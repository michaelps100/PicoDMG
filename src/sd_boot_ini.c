#include "sd_boot_ini.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "ff.h"

/* Panel default, normally supplied by the build. The fallback keeps this file
 * compilable (and sensible) on its own. */
#ifndef DEFAULT_PANEL_ST7789
#define DEFAULT_PANEL_ST7789 0
#endif

/* C-linkage shims over Frens::f_malloc / f_free (FrensHelpers.cpp). Declared
 * here rather than pulled in from a header because FrensHelpers.h is C++ only;
 * this is the same arrangement hstx_data_island_queue.c uses. */
extern void *frens_f_malloc(size_t size);
extern void  frens_f_free(void *ptr);

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void snprintf_err(char *err, size_t err_sz, const char *fmt, ...)
{
    if (!err || err_sz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_sz, fmt, ap);
    va_end(ap);
}

/* Strict unsigned decimal for the diagnostic offsets: digits only, no sign, no
 * trailing junk, no overflow. "007" is fine; "7 " and "0x07" are not (the line
 * is already trimmed, but that is the loader's promise, not this one's). */
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

/* Raw MADCTL register value. Hex, with or without a 0x prefix, so the value
 * maps 1:1 onto the datasheet bit layout the user is dialling in:
 *   MY 0x80  MX 0x40  MV 0x20  ML 0x10  BGR 0x08  MH 0x04
 * Accepts at most two hex digits and nothing else -- "0x28", "28", "A8" are
 * fine; "0x128", "0xZZ" and " 28" (untrimmed) are not. */
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

sd_boot_ini_status_t sd_boot_ini_load(const char *path,
                                      sd_boot_ini_t *out,
                                      char *err, size_t err_sz)
{
    if (err && err_sz) err[0] = '\0';
    if (!out) return SD_BOOT_INI_ERROR;

    /* Fallback defaults (file absent). */
    strcpy(out->base_dir,   "/emu");
    strcpy(out->index_file, "emulators.txt");
    out->screensaver    = SS_MODE_STARFIELD;
    out->gui_graphical  = true;     /* same first-boot default .guimode had */
    out->theme          = 0;
    out->gb_mode        = false;
    out->volume         = 3;
    out->st7789         = (DEFAULT_PANEL_ST7789 != 0);
    out->rotate         = 0;
    out->madctl         = 0;
    out->invert         = false;
    out->st7789_xoff    = 0;
    out->st7789_yoff    = 0;
    out->rumble_pin     = 0;
    out->batt_pin       = 0;
    out->charge_pin     = 0;
    out->power_switch   = 0;
    out->power_pin      = 10;
    strcpy(out->power_behavior, "RESUME");
    out->power_active_low = 1;
    out->file_present   = false;
    out->seen_gui       = false;
    out->seen_theme     = false;
    out->seen_gb_mode   = false;
    out->seen_volume    = false;
    out->seen_st7789    = false;
    out->seen_rotate    = false;
    out->seen_madctl    = false;
    out->seen_invert    = false;
    out->seen_st7789_xoff = false;
    out->seen_st7789_yoff = false;
    out->seen_rumble    = false;
    out->seen_batt_pin  = false;
    out->seen_charge_pin = false;
    out->seen_rtc_epoch = false;
    out->seen_power_switch = false;
    out->seen_power_pin    = false;
    out->seen_power_behavior = false;
    out->seen_power_active_low = false;

    /* Adopt a leftover .tmp when the real file is gone. sd_boot_ini_save()
     * commits with unlink+rename; a power cut in between leaves only the tmp,
     * which is always complete AND already re-parsed at that point. Without
     * this the config would silently revert to "absent" defaults. */
    {
        char tmp[80];
        FILINFO fi;
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        if (f_stat(path, &fi) != FR_OK && f_stat(tmp, &fi) == FR_OK) {
            if (f_rename(tmp, path) == FR_OK) {
                printf("[bootLoader] boot_ini: recovered %s from %s\n", path, tmp);
            }
        }
    }

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        printf("[bootLoader] boot_ini: %s absent, using defaults\n", path);
        return SD_BOOT_INI_OK;
    }
    if (fr != FR_OK) {
        snprintf_err(err, err_sz, "cannot open %s (fr=%d)", path, (int)fr);
        return SD_BOOT_INI_ERROR;
    }

    /* File present: flip screensaver default to BLOCKS per spec. */
    out->screensaver  = SS_MODE_BLOCKS;
    out->file_present = true;

    bool seen_basedir = false;
    bool seen_index   = false;
    bool seen_screen  = false;

    char line[256];
    int lineno = 0;
    while (f_gets(line, sizeof(line), &fil)) {
        lineno++;

        char buf[256];
        size_t n = strlen(line);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, line, n);
        buf[n] = '\0';
        trim(buf);

        if (buf[0] == '\0' || buf[0] == '#' || buf[0] == ';') continue;

        char *eq = strchr(buf, '=');
        if (!eq) {
            snprintf_err(err, err_sz, "missing '=' at line %d", lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }
        *eq = '\0';
        char *key = buf;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (key[0] == '\0') {
            snprintf_err(err, err_sz, "empty key at line %d", lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }
        if (val[0] == '\0') {
            snprintf_err(err, err_sz, "empty value for %s at line %d", key, lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }

        if (strcasecmp(key, "BASEDIR") == 0) {
            if (seen_basedir) {
                snprintf_err(err, err_sz, "duplicate key BASEDIR at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (val[0] != '/') {
                snprintf_err(err, err_sz, "BASEDIR must start with '/' at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            size_t vl = strlen(val);
            if (vl > 1 && val[vl - 1] == '/') { val[vl - 1] = '\0'; vl--; }
            if (vl >= sizeof(out->base_dir)) {
                snprintf_err(err, err_sz, "BASEDIR too long at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            memcpy(out->base_dir, val, vl + 1);
            seen_basedir = true;
        } else if (strcasecmp(key, "INDEX") == 0) {
            if (seen_index) {
                snprintf_err(err, err_sz, "duplicate key INDEX at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (strchr(val, '/') || strchr(val, '\\')) {
                snprintf_err(err, err_sz, "INDEX must be a bare filename at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            size_t vl = strlen(val);
            if (vl >= sizeof(out->index_file)) {
                snprintf_err(err, err_sz, "INDEX too long at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            memcpy(out->index_file, val, vl + 1);
            seen_index = true;
        } else if (strcasecmp(key, "SCREENSAVER") == 0) {
            if (seen_screen) {
                snprintf_err(err, err_sz, "duplicate key SCREENSAVER at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (strcasecmp(val, "BLOCKS") == 0) {
                out->screensaver = SS_MODE_BLOCKS;
            } else if (strcasecmp(val, "STARFIELD") == 0) {
                out->screensaver = SS_MODE_STARFIELD;
            } else {
                snprintf_err(err, err_sz, "invalid SCREENSAVER value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            seen_screen = true;
        } else if (strcasecmp(key, "GUI") == 0) {
            if (out->seen_gui) {
                snprintf_err(err, err_sz, "duplicate key GUI at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid GUI value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->gui_graphical = (val[0] == '1');
            out->seen_gui = true;
        } else if (strcasecmp(key, "THEME") == 0) {
            if (out->seen_theme) {
                snprintf_err(err, err_sz, "duplicate key THEME at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (val[0] < '0' || val[0] > '9' || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid THEME value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->theme = (uint8_t)(val[0] - '0');
            out->seen_theme = true;
        } else if (strcasecmp(key, "GB_MODE") == 0) {
            if (out->seen_gb_mode) {
                snprintf_err(err, err_sz, "duplicate key GB_MODE at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid GB_MODE value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->gb_mode = (val[0] == '1');
            out->seen_gb_mode = true;
        } else if (strcasecmp(key, "VOLUME") == 0) {
            if (out->seen_volume) {
                snprintf_err(err, err_sz, "duplicate key VOLUME at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (val[0] < '0' || val[0] > '5' || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid VOLUME value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->volume = (uint8_t)(val[0] - '0');
            out->seen_volume = true;
        } else if (strcasecmp(key, "ST7789") == 0) {
            if (out->seen_st7789) {
                snprintf_err(err, err_sz, "duplicate key ST7789 at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid ST7789 value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->st7789 = (val[0] == '1');
            out->seen_st7789 = true;
        } else if (strcasecmp(key, "ROTATE") == 0) {
            if (out->seen_rotate) {
                snprintf_err(err, err_sz, "duplicate key ROTATE at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid ROTATE value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->rotate = (uint8_t)(val[0] - '0');
            out->seen_rotate = true;
        } else if (strcasecmp(key, "MADCTL") == 0) {
            uint8_t m;
            if (out->seen_madctl) {
                snprintf_err(err, err_sz, "duplicate key MADCTL at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (!parse_madctl(val, &m)) {
                snprintf_err(err, err_sz, "invalid MADCTL value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->madctl = m;
            out->seen_madctl = true;
        } else if (strcasecmp(key, "INVERT") == 0) {
            if (out->seen_invert) {
                snprintf_err(err, err_sz, "duplicate key INVERT at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid INVERT value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->invert = (val[0] == '1');
            out->seen_invert = true;
        } else if (strcasecmp(key, "ST7789_XOFF") == 0) {
            uint8_t off;
            if (out->seen_st7789_xoff) {
                snprintf_err(err, err_sz, "duplicate key ST7789_XOFF at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (!parse_u8(val, &off)) {
                snprintf_err(err, err_sz, "invalid ST7789_XOFF value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->st7789_xoff = off;
            out->seen_st7789_xoff = true;
        } else if (strcasecmp(key, "ST7789_YOFF") == 0) {
            uint8_t off;
            if (out->seen_st7789_yoff) {
                snprintf_err(err, err_sz, "duplicate key ST7789_YOFF at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (!parse_u8(val, &off)) {
                snprintf_err(err, err_sz, "invalid ST7789_YOFF value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->st7789_yoff = off;
            out->seen_st7789_yoff = true;
        } else if (strcasecmp(key, "RUMBLE") == 0) {
            uint8_t pin;
            /* GPIO for the Game Boy rumble motor. 0 = disabled, which is also
             * the default, so a card that never had this key keeps every other
             * setting in this file. The bootloader does not act on it: the
             * value exists so the key is RECOGNISED, and PicoGBC reads it
             * itself. Without this branch the key would reject the whole file
             * and silently drop GB_MODE, VOLUME and the panel settings. */
            if (out->seen_rumble) {
                snprintf_err(err, err_sz, "duplicate key RUMBLE at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (!parse_u8(val, &pin)) {
                snprintf_err(err, err_sz, "invalid RUMBLE value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->rumble_pin = pin;
            out->seen_rumble = true;
        } else if (strcasecmp(key, "BATT_PIN") == 0) {
            uint8_t pin;
            /* ADC GPIO of the battery voltage divider. 0 = none, the default.
             * Recognised here for the same reason as RUMBLE: the emulators'
             * shared_menu reads it and draws the battery indicator. */
            if (out->seen_batt_pin) {
                snprintf_err(err, err_sz, "duplicate key BATT_PIN at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (!parse_u8(val, &pin)) {
                snprintf_err(err, err_sz, "invalid BATT_PIN value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->batt_pin = pin;
            out->seen_batt_pin = true;
        } else if (strcasecmp(key, "CHARGE_PIN") == 0) {
            uint8_t pin;
            /* GPIO sensing the charger's USB 5 V input (green battery icon).
             * 0 = none, the default. Read by shared_menu, recognised here. */
            if (out->seen_charge_pin) {
                snprintf_err(err, err_sz, "duplicate key CHARGE_PIN at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (!parse_u8(val, &pin)) {
                snprintf_err(err, err_sz, "invalid CHARGE_PIN value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->charge_pin = pin;
            out->seen_charge_pin = true;
        } else if (strcasecmp(key, "RTC_EPOCH") == 0) {
            /* Seed date for the MBC3 real-time clock. Free-form: the value is
             * validated and interpreted by PicoGBC, so the bootloader only
             * records it and leaves the syntax alone. Accepting it here is what
             * matters -- see RUMBLE above. */
            if (out->seen_rtc_epoch) {
                snprintf_err(err, err_sz, "duplicate key RTC_EPOCH at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (val[0] == '\0') {
                snprintf_err(err, err_sz, "empty RTC_EPOCH value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            size_t vl = strlen(val);
            if (vl >= sizeof(out->rtc_epoch)) {
                snprintf_err(err, err_sz, "RTC_EPOCH too long at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            memcpy(out->rtc_epoch, val, vl + 1);
            out->seen_rtc_epoch = true;
        } else if (strcasecmp(key, "POWER_SWITCH") == 0) {
            if (out->seen_power_switch) {
                snprintf_err(err, err_sz, "duplicate key POWER_SWITCH at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid POWER_SWITCH value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->power_switch = (uint8_t)(val[0] - '0');
            out->seen_power_switch = true;
        } else if (strcasecmp(key, "POWER_PIN") == 0) {
            uint8_t pin;
            if (out->seen_power_pin) {
                snprintf_err(err, err_sz, "duplicate key POWER_PIN at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (!parse_u8(val, &pin) || pin > 47) {
                snprintf_err(err, err_sz, "invalid POWER_PIN value at line %d (must be 0..47)", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->power_pin = pin;
            out->seen_power_pin = true;
            if (!out->seen_power_switch) {
                out->power_switch = 1;
            }
        } else if (strcasecmp(key, "POWER_BEHAVIOR") == 0) {
            if (out->seen_power_behavior) {
                snprintf_err(err, err_sz, "duplicate key POWER_BEHAVIOR at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (strcasecmp(val, "RESUME") != 0 && strcasecmp(val, "REBOOT") != 0) {
                snprintf_err(err, err_sz, "invalid POWER_BEHAVIOR value at line %d (must be RESUME or REBOOT)", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            size_t vl = strlen(val);
            if (vl >= sizeof(out->power_behavior)) vl = sizeof(out->power_behavior) - 1;
            memcpy(out->power_behavior, val, vl);
            out->power_behavior[vl] = '\0';
            out->seen_power_behavior = true;
        } else if (strcasecmp(key, "POWER_ACTIVE_LOW") == 0) {
            if (out->seen_power_active_low) {
                snprintf_err(err, err_sz, "duplicate key POWER_ACTIVE_LOW at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid POWER_ACTIVE_LOW value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->power_active_low = (uint8_t)(val[0] - '0');
            out->seen_power_active_low = true;
        } else {
            /* A wrong key name rejects the whole file, so every other setting
             * in it is silently lost too -- the panel then falls back to build
             * defaults and the user sees "nothing I set had any effect". List
             * the valid keys here; the on-screen message is the only feedback
             * available before the display is even configured. */
            snprintf_err(err, err_sz,
                         "unknown key '%s' at line %d -- valid: BASEDIR INDEX "
                         "SCREENSAVER GUI THEME GB_MODE VOLUME ST7789 ROTATE "
                         "MADCTL INVERT ST7789_XOFF ST7789_YOFF RUMBLE RTC_EPOCH BATT_PIN CHARGE_PIN "
                         "POWER_SWITCH POWER_PIN POWER_BEHAVIOR POWER_ACTIVE_LOW",
                         key, lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }
    }
    f_close(&fil);

    /* Shown in the log next to MADCTL, which is only meaningful as an explicit
     * raw value -- printing the derived one here would imply the file carried
     * it. */
    char madctl_str[8];
    if (out->seen_madctl) {
        snprintf(madctl_str, sizeof(madctl_str), "0x%02X", (unsigned)out->madctl);
    } else {
        snprintf(madctl_str, sizeof(madctl_str), "-");
    }

    printf("[bootLoader] boot_ini: %s parsed OK (BASEDIR=%s INDEX=%s SCREENSAVER=%s GUI=%d THEME=%u GB_MODE=%d VOLUME=%u ST7789=%d ROTATE=%u MADCTL=%s INVERT=%d XOFF=%u YOFF=%u RUMBLE=%u BATT_PIN=%u CHARGE_PIN=%u RTC_EPOCH=%s)\n",
           path, out->base_dir, out->index_file,
           out->screensaver == SS_MODE_STARFIELD ? "STARFIELD" : "BLOCKS",
           (int)out->gui_graphical, (unsigned)out->theme, (int)out->gb_mode, (unsigned)out->volume,
           (int)out->st7789, (unsigned)out->rotate,
           out->seen_madctl ? madctl_str : "derived",
           (int)out->invert,
           (unsigned)out->st7789_xoff, (unsigned)out->st7789_yoff,
           (unsigned)out->rumble_pin,
           (unsigned)out->batt_pin,
           (unsigned)out->charge_pin,
           out->seen_rtc_epoch ? out->rtc_epoch : "-");
    return SD_BOOT_INI_OK;
}

/* ------------------------------------------------------------------------
 * Writer
 * ---------------------------------------------------------------------- */

/* Every write goes through f_write. f_puts / f_printf are not referenced
 * anywhere else in this image and the build has no --gc-sections, so calling
 * them would newly link FatFs's own formatter for no benefit. snprintf is
 * already linked all over the place. */
static bool write_raw(FIL *f, const char *s, size_t n)
{
    UINT bw = 0;
    if (n == 0) return true;
    return f_write(f, s, (UINT)n, &bw) == FR_OK && bw == (UINT)n;
}

static bool write_str(FIL *f, const char *s)
{
    return write_raw(f, s, strlen(s));
}

/* 0 = not one of ours, 1 = GUI, 2 = THEME. Non-destructive: `line` is left
 * untouched so it can still be echoed verbatim. */
static int classify(const char *line, char *work, size_t work_sz)
{
    size_t n = 0;
    while (line[n] && line[n] != '=' && n + 1 < work_sz) {
        work[n] = line[n];
        n++;
    }
    if (line[n] != '=') return 0;      /* no '=' on this line */
    work[n] = '\0';
    trim(work);
    if (work[0] == '#' || work[0] == ';') return 0;
    if (strcasecmp(work, "GUI")   == 0) return 1;
    if (strcasecmp(work, "THEME") == 0) return 2;
    if (strcasecmp(work, "VOLUME") == 0) return 3;
    return 0;
}

bool sd_boot_ini_save(const char *path, const sd_boot_ini_t *ini)
{
    if (!path || !ini) return false;

    /* sizeof(FIL) is ~560 bytes here (FF_FS_TINY=0, FF_MAX_SS=512) and we hold
     * two of them open at once, which would not fit the 3 KB stack. One
     * allocation, one free on every exit path. */
    struct SaveScratch {
        FIL  fin;
        FIL  fout;
        char line[256];
        char work[64];
        char out[128];
    };
    struct SaveScratch *sc = (struct SaveScratch *)frens_f_malloc(sizeof(struct SaveScratch));
    if (!sc) {
        printf("[bootLoader] boot_ini: save scratch alloc failed\n");
        return false;
    }

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    if (f_open(&sc->fout, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("[bootLoader] boot_ini: cannot create %s\n", tmp);
        frens_f_free(sc);
        return false;
    }

    bool ok           = true;
    bool wrote_gui    = false;
    bool wrote_theme  = false;
    bool wrote_volume = false;
    bool last_had_eol = true;

    if (f_open(&sc->fin, path, FA_READ) == FR_OK) {
        /* File present: copy through, substituting only our keys. */
        bool continuation = false;
        while (f_gets(sc->line, sizeof(sc->line), &sc->fin)) {
            size_t len     = strlen(sc->line);
            bool   has_eol = (len > 0 && sc->line[len - 1] == '\n');

            /* f_gets splits lines longer than the buffer. The tail chunk is
             * not a line start, so it must never be classified as a key. */
            int kind = continuation ? 0 : classify(sc->line, sc->work, sizeof(sc->work));
            continuation = !has_eol;

            if (kind == 1 && !wrote_gui) {
                snprintf(sc->out, sizeof(sc->out), "GUI=%d\n", ini->gui_graphical ? 1 : 0);
                ok = ok && write_str(&sc->fout, sc->out);
                wrote_gui = true;
            } else if (kind == 2 && !wrote_theme) {
                snprintf(sc->out, sizeof(sc->out), "THEME=%u\n", (unsigned)ini->theme);
                ok = ok && write_str(&sc->fout, sc->out);
                wrote_theme = true;
            } else if (kind == 3 && !wrote_volume) {
                snprintf(sc->out, sizeof(sc->out), "VOLUME=%u\n", (unsigned)ini->volume);
                ok = ok && write_str(&sc->fout, sc->out);
                wrote_volume = true;
            } else if (kind != 0) {
                /* Duplicate key -- drop it. The parser rejects duplicates, so
                 * echoing it would produce a file that fails to load. */
            } else {
                ok = ok && write_raw(&sc->fout, sc->line, len);
            }
            last_had_eol = has_eol;
        }
        f_close(&sc->fin);
    } else {
        /* File absent: materialise every key at its EFFECTIVE value.
         *
         * SCREENSAVER must be written explicitly. sd_boot_ini_load() defaults
         * it to STARFIELD when /boot.txt is absent but to BLOCKS when the file
         * exists without the key, so creating the file while leaving the key
         * out would silently change the screensaver on the next boot. */
        ok = ok && write_str(&sc->fout,
                             "# pico-bootLoader configuration.\n"
                             "# Created by the bootloader. Edit freely -- comments and\n"
                             "# any keys you add are preserved; only GUI and THEME are\n"
                             "# rewritten when you change them from the menu.\n"
                             "# See boot.txt in the project repository for the full reference.\n\n");
        snprintf(sc->out, sizeof(sc->out), "BASEDIR=%s\n", ini->base_dir);
        ok = ok && write_str(&sc->fout, sc->out);
        snprintf(sc->out, sizeof(sc->out), "INDEX=%s\n", ini->index_file);
        ok = ok && write_str(&sc->fout, sc->out);
        snprintf(sc->out, sizeof(sc->out), "SCREENSAVER=%s\n",
                 ini->screensaver == SS_MODE_STARFIELD ? "STARFIELD" : "BLOCKS");
        ok = ok && write_str(&sc->fout, sc->out);
        snprintf(sc->out, sizeof(sc->out), "GB_MODE=%d\n", ini->gb_mode ? 1 : 0);
        ok = ok && write_str(&sc->fout, sc->out);
        snprintf(sc->out, sizeof(sc->out), "VOLUME=%u\n", (unsigned)ini->volume);
        ok = ok && write_str(&sc->fout, sc->out);
        wrote_volume = true;
        /* The panel keys too. Creating /boot.txt without them would make the
         * next boot fall back to DEFAULT_PANEL_ST7789, silently flipping a
         * ST7789 console back to ILI9341 (or, on an ILI9341 build, the other
         * way round) with nothing on screen to explain it. The offsets are
         * diagnostics and stay out unless the user actually set one. */
        snprintf(sc->out, sizeof(sc->out), "ST7789=%d\n", ini->st7789 ? 1 : 0);
        ok = ok && write_str(&sc->fout, sc->out);
        snprintf(sc->out, sizeof(sc->out), "ROTATE=%u\n", (unsigned)ini->rotate);
        ok = ok && write_str(&sc->fout, sc->out);
        /* MADCTL is an explicit override, not a derived value -- writing the
         * derived one out would silently freeze today's default as if the user
         * had picked it. Only an override the user actually wrote is
         * materialised. */
        if (ini->seen_madctl) {
            snprintf(sc->out, sizeof(sc->out), "MADCTL=0x%02X\n", (unsigned)ini->madctl);
            ok = ok && write_str(&sc->fout, sc->out);
        }
        snprintf(sc->out, sizeof(sc->out), "INVERT=%d\n", ini->invert ? 1 : 0);
        ok = ok && write_str(&sc->fout, sc->out);
        if (ini->st7789_xoff || ini->st7789_yoff) {
            snprintf(sc->out, sizeof(sc->out), "ST7789_XOFF=%u\n", (unsigned)ini->st7789_xoff);
            ok = ok && write_str(&sc->fout, sc->out);
            snprintf(sc->out, sizeof(sc->out), "ST7789_YOFF=%u\n", (unsigned)ini->st7789_yoff);
            ok = ok && write_str(&sc->fout, sc->out);
        }
    }

    /* Don't glue an appended key onto a file that ended without a newline. */
    if (!last_had_eol) ok = ok && write_str(&sc->fout, "\n");

    if (!wrote_gui) {
        snprintf(sc->out, sizeof(sc->out),
                 "\n# 0 = text menu, 1 = graphical menu (SELECT toggles).\nGUI=%d\n",
                 ini->gui_graphical ? 1 : 0);
        ok = ok && write_str(&sc->fout, sc->out);
    }
    if (!wrote_theme) {
        snprintf(sc->out, sizeof(sc->out),
                 "\n# Artwork theme 0..9 (UP/DOWN in graphical mode).\nTHEME=%u\n",
                 (unsigned)ini->theme);
        ok = ok && write_str(&sc->fout, sc->out);
    }
    if (!wrote_volume) {
        snprintf(sc->out, sizeof(sc->out),
                 "\n# Sound volume 0..5 (0 = Mute, 5 = Max).\nVOLUME=%u\n",
                 (unsigned)ini->volume);
        ok = ok && write_str(&sc->fout, sc->out);
    }

    if (f_close(&sc->fout) != FR_OK) ok = false;

    /* Validate before committing: reusing the parser costs nothing and makes
     * it impossible to ship a file that would trip the fatal screen. */
    if (ok) {
        sd_boot_ini_t probe;
        char probe_err[64];
        if (sd_boot_ini_load(tmp, &probe, probe_err, sizeof(probe_err)) != SD_BOOT_INI_OK) {
            printf("[bootLoader] boot_ini: refusing to commit %s (%s)\n", tmp, probe_err);
            ok = false;
        }
    }

    if (!ok) {
        f_unlink(tmp);
        frens_f_free(sc);
        return false;
    }

    /* f_rename fails with FR_EXIST if the destination is there. */
    f_unlink(path);
    if (f_rename(tmp, path) != FR_OK) {
        printf("[bootLoader] boot_ini: rename %s -> %s failed\n", tmp, path);
        f_unlink(tmp);
        frens_f_free(sc);
        return false;
    }

    frens_f_free(sc);
    printf("[bootLoader] boot_ini: saved %s (GUI=%d THEME=%u VOLUME=%u)\n",
           path, (int)ini->gui_graphical, (unsigned)ini->theme, (unsigned)ini->volume);
    return true;
}
