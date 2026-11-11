#ifndef DMG_PANEL_H
#define DMG_PANEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* size_t, for dmg_panel_write_fn */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Panel selection, orientation and physical timing, shared by every emulator.
 *
 * The emulators each carry their own copy of the ILI9341/ST7789 driver, and
 * each of those copies had MADCTL hardcoded for the PicoDMG's ILI9341. That
 * value is wrong for the ST7789 on the Waveshare Core2350B, and the resulting
 * symptom (mirrored image) is indistinguishable from a dozen other faults.
 * Centralising the value here means a panel fix is made once instead of nine
 * times, and the emulators cannot drift apart again.
 *
 * The same applies to the refresh overclock below, for the same reason and with
 * a worse failure mode: a driver that forgets it does not look wrong, it just
 * tears during full-screen transitions. See dmg_panel_st7789_timing().
 *
 * The mirror bits are measured, not derived: no datasheet states them, because
 * they depend on how the controller is bonded to the glass. See
 * core2350b_plan.md section 2.5a.
 * =========================================================================
 */

typedef enum {
    DMG_PANEL_ILI9341 = 0,
    DMG_PANEL_ST7789  = 1,
} dmg_panel_t;

/* MADCTL per panel and orientation. Bit layout: MY 0x80, MX 0x40, MV 0x20,
 * ML 0x10, BGR 0x08, MH 0x04.
 *
 * ILI9341 keeps the values this project has always shipped and which are
 * correct for the PicoDMG module, BGR bit included.
 *
 * The ST7789 on the Core2350B is a different bond, and the two axes had to be
 * established separately:
 *
 *   - Mirror (MX): measured first on hardware as 0x68 (i.e. 0x28 plus MX) --
 *     the same landscape orientation with columns reversed, which is the
 *     mirroring that was reported. Measured.
 *   - Colour order (BGR): the ST7789 reads the data as RGB when BGR is clear,
 *     which is the opposite of the ILI9341's behaviour with the same bit. Left
 *     inherited at BGR=1 it swapped red and blue -- 1942's ocean came out red.
 *
 * BGR was never exercised while calibrating: the on-screen calibrator only
 * compares orientations, and 0x28 and (the old) 0x68 differ solely in MX, so
 * every candidate it offered shared one BGR setting. It went unnoticed because
 * the Clean Light UI palette has no saturated red or blue in the ROM list
 * either, so the menu looked correct while the picture did not. Hence BGR is
 * now cleared for the ST7789, which makes the two panels' bit patterns differ
 * in more than orientation.
 *
 * The 180-degree counterpart is the value XOR 0xC0 (toggle MY and MX), not the
 * value OR 0xC0. The two agree on the ILI9341, whose landscape value has
 * neither bit set, which is why the distinction is easy to miss; on the ST7789
 * they give 0xA0 and 0xE0 respectively, and 0xE0 is the ILI9341's inverted
 * value -- the same image mirrored on the wrong axis. */
#define DMG_MADCTL_ILI9341_LANDSCAPE  0x28u
#define DMG_MADCTL_ILI9341_PORTRAIT   0x08u
#define DMG_MADCTL_ST7789_LANDSCAPE   0x60u
#define DMG_MADCTL_ST7789_PORTRAIT    0x40u

/* Rotation values, matching ili9341_set_rotation() across the project. */
#define DMG_ROT_PORTRAIT         0u
#define DMG_ROT_LANDSCAPE        1u
#define DMG_ROT_PORTRAIT_INV     2u
#define DMG_ROT_LANDSCAPE_INV    3u

/* Which controller the build targets when /boot.txt is absent. Must be set by
 * the build so a card with no config still boots onto the right panel. */
#ifndef DMG_DEFAULT_PANEL_ST7789
#define DMG_DEFAULT_PANEL_ST7789 0
#endif

/* Read /boot.txt (ST7789, ROTATE, MADCTL, INVERT, ST7789_XOFF/YOFF) once and
 * cache the result. Must be called after the SD card is mounted.
 *
 * A missing file is not an error: it yields the compile-time panel default and
 * rotation 1, which is what a card without config should do. A malformed file
 * is also non-fatal here -- emulators cannot show a useful diagnostic before
 * the panel is up, so it falls back to defaults and leaves file_present false.
 * The bootloader is the component that turns a bad file into a visible error;
 * by the time an emulator runs, that has already happened or not at all. */
void     dmg_panel_config_init(void);

/* The panel /boot.txt selected, or the compile-time default. */
dmg_panel_t dmg_panel_get(void);

/* The effective MADCTL: the explicit MADCTL= when present, otherwise the
 * value for (panel, rotation). */
uint8_t dmg_panel_get_madctl(void);

/* The value dmg_panel_get_madctl() would have derived, ignoring any explicit
 * MADCTL=. Lets a UI show "the file says 0xA0, the panel would use 0x60". */
uint8_t dmg_panel_get_madctl_derived(void);

/* Whether /boot.txt carried an explicit MADCTL. */
bool dmg_panel_has_explicit_madctl(void);

/* Rotation and colour inversion as requested by /boot.txt. */
uint8_t dmg_panel_get_rotation(void);
bool    dmg_panel_get_invert(void);

/* GRAM offsets, 0 unless /boot.txt overrides them. */
void dmg_panel_get_gram_offset(uint8_t *xoff, uint8_t *yoff);

/* True when /boot.txt was present and parsed. False for a missing or rejected
 * file, which means compile-time defaults are in force. */
bool dmg_panel_file_present(void);

/* Diagnostic for the on-screen/log line: "boot.txt" or "default". */
const char *dmg_panel_source(void);

/* GPIO driving the Game Boy rumble motor, from /boot.txt RUMBLE=. 0 means no
 * motor, which is both the default and what an absent or malformed key yields.
 * The bootloader recognises the key but does not act on it; the emulator does.
 *
 * The pin is free for the user to choose, so this only reports intent. The
 * motor needs a transistor -- it must not be driven straight off the GPIO. */
uint8_t dmg_panel_get_rumble_pin(void);

/* ADC GPIO of the battery voltage divider, from /boot.txt BATT_PIN=. 0 means
 * no battery sensing (default). See dmg_battery.h. */
uint8_t dmg_panel_get_batt_pin(void);

/* GPIO sensing the charger's USB 5 V input, from /boot.txt CHARGE_PIN=.
 * 0 means no charger detection (default). */
uint8_t dmg_panel_get_charge_pin(void);

/* Seed date for the MBC3 real-time clock, from /boot.txt
 * RTC_EPOCH=YYYY-MM-DD[ HH:MM:SS]. NULL when the key is absent or empty, which
 * means "use the built-in reference date".
 *
 * Read once, when the clock is first seeded: the clock itself then lives in the
 * cartridge's .sav and only ever counts forward, so this is a starting point
 * rather than a recurring sync. */
const char *dmg_panel_get_rtc_epoch(void);

/* =========================================================================
 * Physical panel timing
 * =========================================================================
 *
 * The panel sweeps its own glass at a fixed rate, and that rate is not something
 * the emulator controls. On this console the Core 1 blitter needs ~14 ms at
 * 72 MHz SPI to push one frame, while the factory ST7789 sweep takes ~16.6 ms
 * (60 Hz). Those are the same order, so the GRAM write and the beam scan
 * overlap and the overlap is visible as a horizontal tear through whatever is
 * moving -- cutscenes, fades, full-screen scrolling.
 *
 * Overclocking the sweep to ~111 Hz (~9 ms) separates the two. The panel then
 * finishes reading a frame long before the next one is written, so the write
 * happens into a window the beam is not in.
 *
 * This has to be applied in the ST7789 branch of the power-on sequence and
 * never in the ILI9341 one: 0xB2 and 0xC6 are ST7789-only registers, and
 * sending them to an ILI9341 does nothing useful. The converse is also true --
 * 0xB1 (FRMCTR1) is ILI9341-only and must not go to an ST7789.
 *
 * Centralised here because a driver that simply omits it produces no visible
 * error at boot. It looks fine until a fade happens.
 */

/* Combined SPI primitive: send `cmd`, then `len` bytes of parameters. One
 * callback covers both a bare command (len == 0, data may be NULL) and a command
 * with a parameter block.
 *
 * Passed in rather than re-declared so this module stays free of any SPI or
 * pico dependency. PicoGBC's st7789_write_command_data() already has exactly
 * this signature and is passed directly; the ILI9341 drivers, which keep
 * write_cmd() and write_data_buf() separate, pass a four-line adapter. */
typedef void (*dmg_panel_write_fn)(uint8_t cmd, const uint8_t *data, size_t len);

/* Sends the ST7789 refresh overclock: PORCTRL (0xB2) plus FRCTRL2 (0xC6 = 0x01,
 * 111 Hz).
 *
 * Call this from st7789_init_sequence(), before set_addr_window() and before
 * NORON/DISPON. Values are the ones PicoGBC's mk_st7789.c has been running on
 * this glass, which is why they are not "tuned" here. */
void dmg_panel_st7789_timing(dmg_panel_write_fn write);

#ifdef __cplusplus
}
#endif

#endif /* DMG_PANEL_H */
