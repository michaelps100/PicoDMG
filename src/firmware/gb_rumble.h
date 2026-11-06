/*
 * gb_rumble.h - Game Boy rumble motor, driven from a free GPIO.
 *
 * Cartridge types 0x1D and 0x1E are an MBC5 with a vibration motor (Mario
 * Tennis, Perfect Dark, Dragon Warrior Monsters 2). Peanut-GB decodes both as a
 * plain MBC5 and drops bit 3 of the RAM bank select register, which is the
 * motor enable, so those games report "no rumble" and stay silent no matter
 * what hardware is attached. The core now keeps the bit (see peanut_gb.h); this
 * file is the part that turns it into an actual output.
 *
 * The pin comes from /boot.txt RUMBLE=<gpio>, so it can be chosen for the wiring
 * of a particular unit without a rebuild. Absent, zero, or out of range leaves
 * the motor off and this module inert, which is what a card without a motor
 * wants.
 *
 * WIRING: the motor must not be driven straight off the GPIO. Use an NPN or
 * PNP transistor with the motor between the supply and the pin, the usual
 * current-limiting arrangement for a Game Boy rumble motor. The pin here sinks
 * rather than sources (active high into the transistor's base resistor), so
 * RUMBLE= selects a pin that reaches a base resistor.
 */
#ifndef GB_RUMBLE_H
#define GB_RUMBLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pins the rest of the firmware owns. Configuring one of these as a motor
 * output would fight whatever is already using it, so a request for any of
 * them is refused and the motor stays off. */
#define GB_RUMBLE_RESERVED_MASK 0x000003FFu /* GPIO 0..9: buttons */

/* Read RUMBLE= from /boot.txt and claim the pin. Safe to call before the SD
 * card is mounted -- it then reports "no motor", which is the correct answer
 * for a card that cannot be read. Call again after mounting to pick the value
 * up; the second call is the one that matters.
 *
 * Returns the GPIO in use, or 0 when no motor is configured. */
uint8_t gb_rumble_init(void);

/* Drive the motor. `on` is the cartridge's motor-enable bit, which the core
 * exposes as gb.cart_rumble. Passing 0 parks the output, so the motor cannot
 * be left running by a reset or a crash. */
void gb_rumble_set(bool on);

/* Whether a motor is present. The core's motor bit is still tracked when this
 * is false, so a savestate taken on a motor-equipped console and loaded on one
 * without behaves the same; only the physical output is suppressed. */
bool gb_rumble_available(void);

/* Park the motor and release nothing else. Called on the way out of a game so
 * an exit or a reset cannot leave the motor spinning. */
void gb_rumble_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_RUMBLE_H */
