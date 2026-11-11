#ifndef DMG_BATTERY_H
#define DMG_BATTERY_H

/* Battery voltage readout through a 1/2 resistor divider on an ADC pin.
 *
 * Wiring (Core2350B, see AGENTS.md section 42):
 *   B+ -- R1 100k -- GPIO (ADC) -- R2 100k -- OUT- (board GND), 100 nF across R2.
 *
 * The pin comes from /boot.txt BATT_PIN=<gpio>; 0 or absent means no battery
 * hardware and nothing is configured. This module does not read /boot.txt
 * itself, so the bootloader can compile the same file and hand it the pin it
 * parsed on its own. */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the ADC for 'pin'. Safe to call more than once: only the first call
 * with a non-zero pin takes effect. An invalid pin (not ADC-capable on this
 * chip, or one the board already uses) leaves the module disabled and logs why. */
void dmg_battery_init_pin(uint8_t pin);

/* True once a valid pin was configured. Says nothing about a cell being present. */
bool dmg_battery_configured(void);

/* Battery voltage in mV, averaged over 16 samples. 0 when not configured. */
uint16_t dmg_battery_read_mv(void);

/* Below this the reading is treated as "no cell" (USB-only power, divider open). */
#define DMG_BATTERY_PRESENT_MV 2500u

/* Li-ion state of charge, 0..100, from a resting-voltage table. */
uint8_t dmg_battery_percent(uint16_t mv);

/* Charger detection from /boot.txt CHARGE_PIN=<gpio> (0 = none): the TP4056
 * module's IN+ (USB 5 V) through two equal resistors to GND, midpoint on the
 * GPIO. High = charger connected. */
void dmg_battery_init_charge_pin(uint8_t pin);
bool dmg_battery_charge_configured(void);
bool dmg_battery_usb_present(void);

/* Thresholds shared by the menus and the in-game overlay. */
#define DMG_BATTERY_CUTOFF_MV   3300u  /* low-battery standby */
#define DMG_BATTERY_RELEASE_MV  3400u  /* needed to leave it without a charger */
#define DMG_BATTERY_FULL_MV     4150u  /* charger connected and above: full */
#define DMG_BATTERY_LOW_PCT       20u  /* red marker, in-game overlay */

#ifdef __cplusplus
}
#endif

#endif /* DMG_BATTERY_H */
