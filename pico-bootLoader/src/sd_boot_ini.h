/*
 * sd_boot_ini.h - Parse /boot.txt on the SD root.
 *
 * File format (INI-style, optional). One key=value per line.
 * Lines starting with '#' or ';' are comments. Blank lines and CRLF tolerated.
 *
 *   BASEDIR=/uf2                  (default "/emu")
 *   INDEX=uf2list.txt             (default "emulators.txt")
 *   SCREENSAVER=STARFIELD         (BLOCKS or STARFIELD; default: see below)
 *   GUI=1                         (0 = text menu, 1 = graphical; default 1)
 *   THEME=0                       (active artwork theme 0..9; default 0)
 *   GB_MODE=0                     (0 = 320x240, 1 = 266x240 window at x=45)
 *   VOLUME=3                      (0 = Mute .. 5 = Max)
 *   ST7789=1                      (0 = ILI9341, 1 = ST7789; default 0 = DEFAULT_PANEL_ST7789)
 *   ROTATE=0                      (0 = landscape 0 degrees, 1 = landscape 180 degrees)
 *   MADCTL=0x28                   (OPTIONAL raw MADCTL override, hex; see below)
 *   INVERT=0                      (0 = INVOFF, 1 = INVON -- colour inversion for IPS panels)
 *   ST7789_XOFF=0                 (OPTIONAL diagnostic: GRAM column offset)
 *   ST7789_YOFF=0                 (OPTIONAL diagnostic: GRAM row offset)
 *   RUMBLE=0                      (PicoGBC: GPIO for the rumble motor; 0 = off)
 *   BATT_PIN=0                    (shared_menu: ADC GPIO of the battery divider; 0 = off)
 *   CHARGE_PIN=0                  (shared_menu: GPIO sensing the charger 5 V; 0 = off)
 *   RTC_EPOCH=2000-01-01 00:00:00 (PicoGBC: seed date for the MBC3 clock)
 *
 * RUMBLE and RTC_EPOCH are consumed by PicoGBC, not by the bootloader. They are
 * recognised here only so their presence does not reject the file: this parser
 * is a whitelist, so an unknown key discards every other setting along with it.
 * Both default to "off"/"unset", so a card without them behaves exactly as
 * before.
 *
 * BASEDIR is the SD-root folder under which the bootloader looks for the
 * per-HW_CONFIG subdir, the allow-list, and the assets folder.
 * INDEX is the filename of the allow-list inside BASEDIR.
 * SCREENSAVER selects the idle visual mode.
 * GUI and THEME are written back by the firmware -- see sd_boot_ini_save().
 *
 * ST7789 / ROTATE / INVERT select the SPI panel and its orientation. They are
 * read *before* the display is initialised (see the pre-display hook in
 * main.cpp), which is why they are parsed by this file rather than by the
 * display driver: with no card present every one of them falls back to the
 * compile-time DEFAULT_PANEL_ST7789 and the plain 0/0 defaults, so a missing
 * card can never leave the panel mismatched and the fatal screens unreadable.
 * ST7789_XOFF / ST7789_YOFF are diagnostics for a panel whose GRAM window is
 * offset; 0/0 is correct for the module this project uses.
 *
 * MADCTL is an escape hatch, not a normal setting. ROTATE covers the two
 * orientations the panel is expected to need, and it is what sd_boot_ini_save()
 * keeps writing. MADCTL exists because ST7789 modules disagree on the mirror
 * bits far more often than on the rotation, and a wrong mirror is not
 * diagnosable from the picture alone. When the key is present it wins over
 * ROTATE and is applied verbatim, 1:1 with the datasheet bit layout:
 *
 *   MY 0x80  row address order     (vertical mirror)
 *   MX 0x40  column address order  (horizontal mirror)
 *   MV 0x20  row/column exchange   (the 90/270 degree swap)
 *   ML 0x10  data latch
 *   BGR 0x08 colour order          (0 = RGB, 1 = BGR)
 *   MH 0x04  horizontal refresh
 *
 * To find the right value, start from the one in the boot log and add MY
 * (0x80) for a top/bottom flip, MX (0x40) for a left/right flip, or both
 * (0xC0) for a 180 degree flip. Only the two geometry bits are usually wrong;
 * leave MV and BGR alone unless the picture is also the wrong size or the
 * wrong colours. When the key is absent the driver keeps deriving MADCTL from
 * ROTATE, and save() does not invent the key.
 *
 * Screensaver default is asymmetric:
 *   - When /boot.txt is absent altogether -> STARFIELD (historical default).
 *   - When /boot.txt is present but has no SCREENSAVER key -> BLOCKS.
 *   - Explicit SCREENSAVER= wins.
 * This is exactly why sd_boot_ini_save() writes SCREENSAVER= explicitly when it
 * has to create the file: materialising /boot.txt on a card that never had one
 * would otherwise silently flip the screensaver from STARFIELD to BLOCKS.
 *
 * Return values:
 *   SD_BOOT_INI_OK    - out[] is populated (either defaults, or parsed).
 *   SD_BOOT_INI_ERROR - a parse rule failed. err[] holds a single-line
 *                       diagnostic ready to display on a fatal screen.
 */
#ifndef SD_BOOT_INI_H
#define SD_BOOT_INI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SS_MODE_BLOCKS    = 0,   /* bouncing sprites */
    SS_MODE_STARFIELD = 1,   /* pin-hole depth projection */
} ss_mode_t;

typedef struct {
    char      base_dir[64];    /* no trailing slash, e.g. "/emu" */
    char      index_file[64];  /* bare filename, e.g. "emulators.txt" */
    ss_mode_t screensaver;
    bool      gui_graphical;   /* GUI=0|1;   default true (graphical) */
    uint8_t   theme;           /* THEME=0..9; default 0 */
    bool      gb_mode;         /* GB_MODE=0|1; default false (0 = 320x240, 1 = 266x240 @ x=45) */
    uint8_t   volume;          /* VOLUME=0..5; default 3 (0 = Mute, 5 = Max) */

    /* Panel selection. Defaults come from DEFAULT_PANEL_ST7789 so that a
     * console built for one panel still boots on it with no card inserted. */
    bool      st7789;          /* ST7789=0|1; default = DEFAULT_PANEL_ST7789 */
    uint8_t   rotate;          /* ROTATE=0|1; default 0 (landscape 0 degrees) */
    bool      invert;          /* INVERT=0|1; default 0 (0 = INVOFF, 1 = INVON) */
    uint8_t   st7789_xoff;     /* GRAM column offset; default 0 */
    uint8_t   st7789_yoff;     /* GRAM row offset; default 0 */
    uint8_t   madctl;          /* MADCTL=0xNN override; only meaningful if seen_madctl */

    /* Settings owned by an emulator rather than by the bootloader.
     *
     * The bootloader records these but takes no action on them. They are parsed
     * here purely so the keys are RECOGNISED: this loader is a whitelist, and an
     * unrecognised key rejects the whole file, which would silently discard
     * GB_MODE, VOLUME and every panel setting in it. PicoGBC re-reads the file
     * itself and acts on them. */
    uint8_t   rumble_pin;      /* RUMBLE=<gpio>; 0 = motor disabled, the default */
    uint8_t   batt_pin;        /* BATT_PIN=<gpio>; 0 = no battery sensing, the default */
    uint8_t   charge_pin;      /* CHARGE_PIN=<gpio>; 0 = no charger detection, the default */
    char      rtc_epoch[24];   /* RTC_EPOCH=YYYY-MM-DD[ HH:MM:SS]; "" = unset */
    uint8_t   power_switch;    /* POWER_SWITCH=0|1; 0 = disabled, the default */
    uint8_t   power_pin;       /* POWER_PIN=0..29; 10 = default GP10 */
    char      power_behavior[16]; /* POWER_BEHAVIOR=RESUME|REBOOT; default "RESUME" */
    uint8_t   power_active_low;/* POWER_ACTIVE_LOW=0|1; default 1 (GND=ON, Open=OFF) */

    /* Provenance, filled in by the loader. Not user-visible configuration --
     * the writer and the .guimode migration both need to know what was
     * actually present in the file as opposed to defaulted. */
    bool      file_present;
    bool      seen_gui;
    bool      seen_theme;
    bool      seen_gb_mode;
    bool      seen_volume;
    bool      seen_st7789;
    bool      seen_rotate;
    bool      seen_invert;
    bool      seen_st7789_xoff;
    bool      seen_st7789_yoff;
    bool      seen_madctl;
    bool      seen_rumble;
    bool      seen_batt_pin;
    bool      seen_charge_pin;
    bool      seen_rtc_epoch;
    bool      seen_power_switch;
    bool      seen_power_pin;
    bool      seen_power_behavior;
    bool      seen_power_active_low;
} sd_boot_ini_t;

typedef enum {
    SD_BOOT_INI_OK,
    SD_BOOT_INI_ERROR,
} sd_boot_ini_status_t;

/* Parse `path`. Also adopts a leftover "<path>.tmp" when `path` itself is
 * missing -- that is the one window sd_boot_ini_save() cannot close by
 * itself (a power cut between unlink and rename), and letting the file
 * silently revert to "absent" would re-fire the SCREENSAVER asymmetry. */
sd_boot_ini_status_t sd_boot_ini_load(const char *path,
                                      sd_boot_ini_t *out,
                                      char *err, size_t err_sz);

/* Rewrite `path` so GUI= and THEME= reflect *ini.
 *
 * File present: only the GUI= / THEME= / VOLUME= lines are rewritten, and keys
 *   that are absent get appended. Everything else -- comments, blank lines,
 *   ordering, spacing, CRLF, BASEDIR/INDEX/SCREENSAVER, and the panel keys
 *   ST7789/ROTATE/INVERT -- is copied through byte for byte. The panel keys are
 *   hand-edited, so rewriting them would only risk clobbering a deliberate
 *   ST7789=1 on a card that also happens to hold an ILI9341 build.
 * File absent: the file is created carrying the EFFECTIVE value of every key,
 *   SCREENSAVER included (see the asymmetry note at the top of this header).
 *   ST7789/ROTATE/INVERT are written too, so materialising /boot.txt can never
 *   silently move a ST7789 console back to the DEFAULT_PANEL_ST7789 fallback.
 *
 * The new file is built as "<path>.tmp", re-parsed with sd_boot_ini_load()
 * before being committed, and only then unlinked+renamed into place -- so a
 * formatting bug or a power cut can never leave /boot.txt in a state that
 * trips the BOOT.TXT INVALID fatal screen. On any failure the original file
 * is left untouched.
 *
 * Returns true on success. Callers should treat failure as non-fatal: a
 * write-protected or full card must not stop the picker from working. */
bool sd_boot_ini_save(const char *path, const sd_boot_ini_t *ini);

#ifdef __cplusplus
}
#endif

#endif /* SD_BOOT_INI_H */
