#include "dmg_panel.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "ff.h"

/* =========================================================================
 * Panel configuration from /boot.txt
 *
 * The bootloader owns the authoritative parse (src/sd_boot_ini.c) and refuses
 * to boot on a malformed file. This is a deliberately smaller, non-fatal
 * reader: an emulator only needs ST7789 / ROTATE / MADCTL / INVERT and must
 * never be the reason a good card fails to start. It looks for the same
 * "KEY=VALUE" lines and ignores everything it does not recognise, including
 * whole unknown keys, rather than rejecting the file the way the loader does.
 * =========================================================================
 */

static bool     s_initialised = false;
static bool     s_file_present = false;

static dmg_panel_t s_panel = (DMG_DEFAULT_PANEL_ST7789 != 0) ? DMG_PANEL_ST7789
                                                             : DMG_PANEL_ILI9341;
static uint8_t  s_rotate = 0;          /* /boot.txt ROTATE=0|1 */
static bool     s_invert = false;
static uint8_t  s_xoff = 0;
static uint8_t  s_yoff = 0;
static bool     s_has_madctl = false;
static uint8_t  s_madctl = 0;

/* Emulator-owned keys. These are the same lines the bootloader now recognises
 * (see sd_boot_ini.c); it records them without acting on them, and PicoGBC acts
 * on them here. Both default to "off"/"unset" so a card without the keys is
 * unaffected. */
static uint8_t  s_rumble_pin = 0;              /* /boot.txt RUMBLE=<gpio> */
static uint8_t  s_batt_pin = 0;                /* /boot.txt BATT_PIN=<gpio> */
static uint8_t  s_charge_pin = 0;              /* /boot.txt CHARGE_PIN=<gpio> */
static char     s_rtc_epoch[24] = {0};         /* /boot.txt RTC_EPOCH=... */
static bool     s_has_rtc_epoch = false;

/* Raw MADCTL, hex with or without 0x. At most two digits, nothing else, so a
 * typo cannot silently become a plausible-looking value. */
static bool parse_madctl(const char *s, uint8_t *out)
{
    if (!s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (s[0] == '\0' || s[1] == '\0' || s[2] != '\0') return false;
    unsigned v = 0;
    for (const char *p = s; *p; p++) {
        unsigned d;
        if (*p >= '0' && *p <= '9')      d = (unsigned)(*p - '0');
        else if (*p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a') + 10u;
        else if (*p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A') + 10u;
        else return false;
        v = (v << 4) | d;
    }
    *out = (uint8_t)v;
    return true;
}

static bool parse_u8(const char *s, uint8_t *out)
{
    if (!s || s[0] == '\0') return false;
    unsigned v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 255u) return false;
    }
    *out = (uint8_t)v;
    return true;
}

/* MADCTL for the panel and /boot.txt's ROTATE=0|1. The portrait orientations are
 * not reachable from /boot.txt (ROTATE only offers the two landscape ones), so
 * they stay in dmg_panel.h as constants for any caller that drives MADCTL
 * directly rather than through this helper. */
static uint8_t madctl_for(dmg_panel_t panel, uint8_t rotate)
{
    const uint8_t landscape = (panel == DMG_PANEL_ST7789) ? DMG_MADCTL_ST7789_LANDSCAPE
                                                         : DMG_MADCTL_ILI9341_LANDSCAPE;
    /* 180 degrees toggles MY and MX. XOR, not OR: on the ILI9341 (0x28, neither
     * bit set) the two agree, but on the ST7789 (0x60, MX already set) OR would
     * give 0xE0 -- the ILI9341's inverted value, mirrored on the wrong axis --
     * where XOR gives 0xA0, the ST7789's own inverted value. */
    return rotate ? (uint8_t)(landscape ^ 0xC0u) : landscape;
}

/* Maps /boot.txt's ROTATE=0|1 onto the driver's rotation index, matching the
 * bootloader's applyBootPanelConfig(): ROTATE=0 is landscape (1), ROTATE=1 is
 * landscape inverted (3). */
static uint8_t rotation_for(uint8_t rotate)
{
    return rotate ? 3u : 1u;
}

void dmg_panel_config_init(void)
{
    if (s_initialised) return;
    static const char *const paths[] = {
        "0:/boot.txt", "/boot.txt", "boot.txt", "0:boot.txt",
    };
    const unsigned npaths = (unsigned)(sizeof(paths) / sizeof(paths[0]));

    FIL fil;
    bool opened = false;
    FRESULT fr = FR_NOT_READY;
    for (unsigned i = 0; i < npaths; i++) {
        fr = f_open(&fil, paths[i], FA_READ);
        if (fr == FR_OK) { opened = true; break; }
    }
    if (!opened) {
        if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
            s_initialised = true;
        }
        return;   /* no card, or no boot.txt: compile-time defaults stand */
    }

    s_initialised = true;
    s_file_present = true;

    char line[192];
    while (f_gets(line, sizeof(line), &fil)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;

        /* trim key tail */
        size_t kl = strlen(p);
        while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) p[--kl] = '\0';
        /* trim value both ends */
        while (*val == ' ' || *val == '\t') val++;
        size_t vl = strlen(val);
        while (vl > 0 && (val[vl - 1] == ' ' || val[vl - 1] == '\t' ||
                          val[vl - 1] == '\r' || val[vl - 1] == '\n')) {
            val[--vl] = '\0';
        }
        if (*val == '\0') continue;

        if (strcasecmp(p, "ST7789") == 0) {
            if (val[0] == '0' || val[0] == '1') s_panel = (val[0] == '1') ? DMG_PANEL_ST7789
                                                                          : DMG_PANEL_ILI9341;
        } else if (strcasecmp(p, "ROTATE") == 0) {
            if (val[0] == '0' || val[0] == '1') s_rotate = (uint8_t)(val[0] - '0');
        } else if (strcasecmp(p, "INVERT") == 0) {
            s_invert = (val[0] == '1');
        } else if (strcasecmp(p, "MADCTL") == 0) {
            uint8_t m;
            if (parse_madctl(val, &m)) { s_madctl = m; s_has_madctl = true; }
        } else if (strcasecmp(p, "ST7789_XOFF") == 0) {
            uint8_t v;
            if (parse_u8(val, &v)) s_xoff = v;
        } else if (strcasecmp(p, "ST7789_YOFF") == 0) {
            uint8_t v;
            if (parse_u8(val, &v)) s_yoff = v;
        } else if (strcasecmp(p, "RUMBLE") == 0) {
            /* 0 = no motor. Rejected as 0 so a malformed value leaves the
             * default (off) rather than enabling a pin nobody chose. */
            uint8_t v;
            if (parse_u8(val, &v)) s_rumble_pin = v;
        } else if (strcasecmp(p, "BATT_PIN") == 0) {
            /* ADC pin of the battery divider. 0 = none. Validity against the
             * chip and the board wiring is dmg_battery.c's job. */
            uint8_t v;
            if (parse_u8(val, &v)) s_batt_pin = v;
        } else if (strcasecmp(p, "CHARGE_PIN") == 0) {
            /* GPIO sensing the charger's 5 V input. 0 = none. */
            uint8_t v;
            if (parse_u8(val, &v)) s_charge_pin = v;
        } else if (strcasecmp(p, "RTC_EPOCH") == 0) {
            /* Kept verbatim: the syntax is interpreted where the clock is
             * seeded, so there is nothing to validate here beyond a length
             * that fits. */
            if (vl < sizeof(s_rtc_epoch)) {
                memcpy(s_rtc_epoch, val, vl + 1);
                s_has_rtc_epoch = (vl > 0);
            }
        }
        /* Unknown keys are ignored on purpose. The loader already reports them
         * fatally at boot, so by the time an emulator runs the file is known
         * good; rejecting it again here would only add a failure mode. */
    }
    f_close(&fil);
}

dmg_panel_t dmg_panel_get(void)
{
    dmg_panel_config_init();
    return s_panel;
}

uint8_t dmg_panel_get_madctl(void)
{
    dmg_panel_config_init();
    return s_has_madctl ? s_madctl : madctl_for(s_panel, s_rotate);
}

uint8_t dmg_panel_get_madctl_derived(void)
{
    dmg_panel_config_init();
    return madctl_for(s_panel, s_rotate);
}

bool dmg_panel_has_explicit_madctl(void)
{
    dmg_panel_config_init();
    return s_has_madctl;
}

uint8_t dmg_panel_get_rotation(void)
{
    dmg_panel_config_init();
    return rotation_for(s_rotate);
}

bool dmg_panel_get_invert(void)
{
    dmg_panel_config_init();
    return s_invert;
}

void dmg_panel_get_gram_offset(uint8_t *xoff, uint8_t *yoff)
{
    dmg_panel_config_init();
    if (xoff) *xoff = s_xoff;
    if (yoff) *yoff = s_yoff;
}

bool dmg_panel_file_present(void)
{
    dmg_panel_config_init();
    return s_file_present;
}

const char *dmg_panel_source(void)
{
    dmg_panel_config_init();
    return s_file_present ? "boot.txt" : "default";
}

uint8_t dmg_panel_get_batt_pin(void)
{
    dmg_panel_config_init();
    return s_batt_pin;
}

uint8_t dmg_panel_get_charge_pin(void)
{
    dmg_panel_config_init();
    return s_charge_pin;
}

uint8_t dmg_panel_get_rumble_pin(void)
{
    dmg_panel_config_init();
    return s_rumble_pin;
}

const char *dmg_panel_get_rtc_epoch(void)
{
    dmg_panel_config_init();
    return s_has_rtc_epoch ? s_rtc_epoch : NULL;
}

/* ST7789 refresh overclock. PORCTRL sets the porches (tightened back porch to
 * shorten the sweep), FRCTRL2 selects 111 Hz instead of the 60 Hz default.
 *
 * These are ST7789-only registers. 0xB1 (FRMCTR1) is ILI9341-only and must not
 * be sent here; see the header for why the two directions are both real. */
#define DMG_ST7789_PORCTRL  0xB2u
#define DMG_ST7789_FRCTRL2  0xC6u

void dmg_panel_st7789_timing(dmg_panel_write_fn write)
{
    /* BPA=12, FPA=12, PSEN=0, BPB=0x33, FPB=0x33 */
    static const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    /* 0x0F is the 60 Hz default; 0x01 is 111 Hz. */
    static const uint8_t frctrl2[] = {0x01};

    if (write == NULL) return;
    write(DMG_ST7789_PORCTRL, porctrl, sizeof(porctrl));
    write(DMG_ST7789_FRCTRL2, frctrl2, sizeof(frctrl2));
}
