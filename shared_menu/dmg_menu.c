#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"

#include "ff.h"
#include "dmg_menu.h"
#include "dmg_battery.h"
#include "dmg_boot_intro.h"
#include "dmg_power.h"

/* =========================================================================
 * Clean Light UI Palettes
 * ========================================================================= */
static const dmg_palette_t s_pal_rgb565 = {
    .bg         = DMG_RGB565_BG_COLOR,
    .bar        = DMG_RGB565_BAR_COLOR,
    .border     = DMG_RGB565_BORDER_COLOR,
    .text       = DMG_RGB565_TEXT_COLOR,
    .text_bar   = DMG_RGB565_TEXT_BAR_COLOR,
    .text_sel   = DMG_RGB565_TEXT_SEL_COLOR,
    .sel        = DMG_RGB565_SEL_COLOR,
    .status_ok  = DMG_RGB565_STATUS_OK,
    .status_err = DMG_RGB565_STATUS_ERR
};

static const dmg_palette_t s_pal_rgb555 = {
    .bg         = DMG_RGB555_BG_COLOR,
    .bar        = DMG_RGB555_BAR_COLOR,
    .border     = DMG_RGB555_BORDER_COLOR,
    .text       = DMG_RGB555_TEXT_COLOR,
    .text_bar   = DMG_RGB555_TEXT_BAR_COLOR,
    .text_sel   = DMG_RGB555_TEXT_SEL_COLOR,
    .sel        = DMG_RGB555_SEL_COLOR,
    .status_ok  = DMG_RGB555_STATUS_OK,
    .status_err = DMG_RGB555_STATUS_ERR
};

void dmg_get_palette(const dmg_emulator_desc_t *desc, dmg_palette_t *out_pal) {
    if (out_pal == NULL) return;
    if (desc && desc->is_rgb555) {
        *out_pal = s_pal_rgb555;
    } else {
        *out_pal = s_pal_rgb565;
    }
}

/* =========================================================================
 * Hardware Controls (GPIO 2..9)
 * ========================================================================= */
void dmg_init_ui_buttons(void) {
    static bool inited = false;
    if (!inited) {
        for (int pin = 2; pin <= 9; pin++) {
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_IN);
            gpio_pull_up(pin);
        }
        inited = true;
    }
}

void dmg_read_ui_buttons(bool *up, bool *down, bool *left, bool *right,
                         bool *a, bool *b, bool *select, bool *start) {
    dmg_init_ui_buttons();
    dmg_power_poll(NULL);
    if (up)     *up     = !gpio_get(2);
    if (down)   *down   = !gpio_get(3);
    if (left)   *left   = !gpio_get(4);
    if (right)  *right  = !gpio_get(5);
    if (a)      *a      = !gpio_get(6);
    if (b)      *b      = !gpio_get(7);
    if (select) *select = !gpio_get(8);
    if (start)  *start  = !gpio_get(9);
}

void dmg_wait_for_all_buttons_released(void) {
    dmg_init_ui_buttons();
    int timeout = 50; /* max 50 * 15ms = 750ms */
    while (timeout-- > 0) {
        watchdog_update();
        if (gpio_get(2) && gpio_get(3) && gpio_get(4) && gpio_get(5) &&
            gpio_get(6) && gpio_get(7) && gpio_get(8) && gpio_get(9)) {
            break;
        }
        sleep_ms(15);
    }
}

/* =========================================================================
 * Bootloader Handshake
 * ========================================================================= */
#define LOADER_LAUNCH_MAGIC 0xB007ED01u
#define LOADER_RETURN_MAGIC 0xB007BACEu

bool dmg_is_launched_from_bootloader(void) {
    return (watchdog_hw->scratch[6] == LOADER_LAUNCH_MAGIC);
}

void dmg_reboot_to_bootloader(void) {
    printf("[shared_menu] Returning to bootloader...\n");
    watchdog_hw->scratch[7] = LOADER_RETURN_MAGIC;
    watchdog_reboot(0, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}

/* =========================================================================
 * Boot Mode Management (GB_MODE in 0:/boot.txt)
 * ========================================================================= */
static bool s_gb_mode = false;
static bool s_boot_mode_read = false;
static uint8_t s_global_volume = 3;
static bool s_global_volume_seen = false;
static bool s_global_volume_dirty = false;
static FIL s_menu_fil;

void dmg_init_boot_mode(void) {
    if (s_boot_mode_read) return;

    FRESULT fr = f_open(&s_menu_fil, "0:/boot.txt", FA_READ);
    if (fr != FR_OK) {
        fr = f_open(&s_menu_fil, "/boot.txt", FA_READ);
    }
    if (fr != FR_OK) {
        fr = f_open(&s_menu_fil, "boot.txt", FA_READ);
    }
    if (fr != FR_OK) {
        fr = f_open(&s_menu_fil, "0:boot.txt", FA_READ);
    }

    if (fr != FR_OK) {
        /* Only cache the miss if the drive was actually mounted and confirmed
         * the file does not exist (FR_NO_FILE or FR_NO_PATH). If the card is not
         * ready (FR_NOT_READY, FR_INVALID_DRIVE, etc.), do NOT cache, so
         * dmg_init_boot_mode() can succeed once the drive is mounted! */
        if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
            s_boot_mode_read = true;
            s_gb_mode = false;
        }
        return;
    }

    {
        s_boot_mode_read = true;
        s_gb_mode = false;
        char buf[512];
        UINT br = 0;
        fr = f_read(&s_menu_fil, buf, sizeof(buf) - 1, &br);
        if (fr == FR_OK && br > 0) {
            buf[br] = '\0';
            char *line = buf;
            while (line && *line) {
                char *next_line = strpbrk(line, "\r\n");
                if (next_line) {
                    *next_line = '\0';
                    next_line++;
                    if (*next_line == '\r' || *next_line == '\n') next_line++;
                }
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '#' && *p != ';' && *p != '\0') {
                    if (strncasecmp(p, "GB_MODE", 7) == 0) {
                        char *eq = strchr(p, '=');
                        if (eq) {
                            eq++;
                            while (*eq == ' ' || *eq == '\t') eq++;
                            if (*eq == '1') {
                                s_gb_mode = true;
                            }
                        }
                    } else if (strncasecmp(p, "VOLUME", 6) == 0) {
                        char *eq = strchr(p, '=');
                        if (eq) {
                            eq++;
                            while (*eq == ' ' || *eq == '\t') eq++;
                            if (*eq >= '0' && *eq <= '5') {
                                s_global_volume = (uint8_t)(*eq - '0');
                                s_global_volume_seen = true;
                            }
                        }
                    }
                }
                line = next_line;
            }
        }
        f_close(&s_menu_fil);
        printf("[shared_menu] Boot mode initialized from boot.txt: GB_MODE=%d VOLUME=%u\n",
               (int)s_gb_mode, (unsigned)s_global_volume);
    }
}

uint8_t dmg_get_global_volume(void) {
    dmg_init_boot_mode();
    return s_global_volume;
}

static void dmg_save_global_volume_file(uint8_t vol) {
    if (vol > 5) vol = 5;
    s_global_volume = vol;
    s_global_volume_seen = true;

    static char s_content[1024];
    UINT br = 0;
    s_content[0] = '\0';

    FRESULT fr = f_open(&s_menu_fil, "0:/boot.txt", FA_READ);
    if (fr != FR_OK) fr = f_open(&s_menu_fil, "/boot.txt", FA_READ);
    if (fr != FR_OK) fr = f_open(&s_menu_fil, "boot.txt", FA_READ);
    if (fr == FR_OK) {
        fr = f_read(&s_menu_fil, s_content, sizeof(s_content) - 64, &br);
        f_close(&s_menu_fil);
        if (fr == FR_OK && br < sizeof(s_content) - 64) {
            s_content[br] = '\0';
        } else {
            s_content[0] = '\0';
        }
    }

    /* Written back line by line, straight from s_content: a second 1 KB
     * buffer for the rebuilt file was SRAM PicoWS could not spare. */
    fr = f_open(&s_menu_fil, "0:/boot.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) fr = f_open(&s_menu_fil, "/boot.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) return;

    UINT bw = 0;
    bool wrote_vol = false;
    char vline[32];
    snprintf(vline, sizeof(vline), "VOLUME=%u\n", (unsigned)vol);

    if (s_content[0] != '\0') {
        char *line = s_content;
        while (line && *line) {
            char *next_line = strpbrk(line, "\r\n");
            if (next_line) {
                *next_line = '\0';
                next_line++;
                if (*next_line == '\r' || *next_line == '\n') next_line++;
            }
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncasecmp(p, "VOLUME", 6) == 0 && (p[6] == '=' || p[6] == ' ' || p[6] == '\t')) {
                f_write(&s_menu_fil, vline, strlen(vline), &bw);
                wrote_vol = true;
            } else {
                f_write(&s_menu_fil, line, strlen(line), &bw);
                f_write(&s_menu_fil, "\n", 1, &bw);
            }
            line = next_line;
        }
    }
    if (!wrote_vol) f_write(&s_menu_fil, vline, strlen(vline), &bw);
    f_close(&s_menu_fil);
    printf("[shared_menu] Saved global VOLUME=%u to boot.txt\n", (unsigned)vol);
}

bool dmg_is_gb_mode(void) {
    dmg_init_boot_mode();
    return s_gb_mode;
}

uint16_t dmg_get_window_x(void) {
    return dmg_is_gb_mode() ? DMG_GB_WINDOW_X : 0u;
}

uint16_t dmg_get_window_w(void) {
    return dmg_is_gb_mode() ? DMG_GB_WINDOW_W : 320u;
}

/* =========================================================================
 * Persistent Configuration
 * ========================================================================= */
dmg_config_t g_dmg_config = {
    .magic            = 0,
    .version          = 1,
    .scale_mode       = DMG_SCALE_FIT,
    .grid_effect      = 0,
    .interleave       = 0,
    .turbo_mode       = 0,
    .volume_level     = 3,
    .brightness_level = 5,
    .save_slot        = 1,
    .load_slot        = 1,
    .extra_opt1       = 0,
    .extra_opt2       = 0
};

void dmg_config_load(const dmg_emulator_desc_t *desc) {
    dmg_init_boot_mode();
    if (desc == NULL || desc->config_path == NULL) return;

    FRESULT fr = f_open(&s_menu_fil, desc->config_path, FA_READ);
    if (fr != FR_OK && desc->config_fallback) {
        fr = f_open(&s_menu_fil, desc->config_fallback, FA_READ);
    }

    if (fr == FR_OK) {
        /* Zero-initialised, and that is not cosmetic.
         *
         * Config files written by an older build can be shorter than the struct:
         * I:\saves\ms\picosms.cfg on the card right now is 44 bytes against a
         * 48-byte dmg_config_t. f_read() reports the short count and leaves the
         * tail untouched, so with `dmg_config_t loaded;` the last field came
         * from whatever was on the stack -- and that is not theoretical, it is
         * what the cards show: extra_opt1 reads "it f", "ader" and "Donk" in
         * the PCE, MD and A26 files. With the field now carrying the FPS
         * overlay, garbage there means "non-zero", i.e. the overlay forced on. */
        dmg_config_t loaded;
        memset(&loaded, 0, sizeof(loaded));
        UINT br = 0;
        fr = f_read(&s_menu_fil, &loaded, sizeof(loaded), &br);
        f_close(&s_menu_fil);

        if (fr == FR_OK && br >= (sizeof(uint32_t) * 10) && loaded.magic == desc->config_magic) {
            g_dmg_config = loaded;
            /* Clamp values. A short file leaves zeros in the tail, which are
             * legal-looking but wrong (save_slot 0, turbo 0), so the same
             * range checks run on those fields as on the rest. */
            if (g_dmg_config.scale_mode > 2) g_dmg_config.scale_mode = DMG_SCALE_FIT;
            if (g_dmg_config.volume_level > 5) g_dmg_config.volume_level = 3;
            if (s_global_volume_seen) g_dmg_config.volume_level = s_global_volume;
            if (g_dmg_config.brightness_level < 1 || g_dmg_config.brightness_level > 5) g_dmg_config.brightness_level = 5;
            if (g_dmg_config.save_slot < 1 || g_dmg_config.save_slot > 4) g_dmg_config.save_slot = 1;
            if (g_dmg_config.load_slot < 1 || g_dmg_config.load_slot > 4) g_dmg_config.load_slot = 1;
            /* turbo_mode is used as a boolean by every apply_speed(). A file
             * with 3 in there (which is what the stale 44-byte SMS file holds)
             * would otherwise read as "turbo on" with nothing to turn it off. */
            if (g_dmg_config.turbo_mode > 1u) g_dmg_config.turbo_mode = 0u;
            if (g_dmg_config.grid_effect > 1u) g_dmg_config.grid_effect = 0u;
            if (g_dmg_config.interleave > 1u) g_dmg_config.interleave = 0u;
            if (g_dmg_config.extra_opt1 > 1u) g_dmg_config.extra_opt1 = 0u;

            if (br < sizeof(loaded)) {
                printf("[shared_menu] %s config is %u bytes, expected %u -- "
                       "written by an older build. Tail fields defaulted.\n",
                       desc->app_title ? desc->app_title : "App",
                       (unsigned)br, (unsigned)sizeof(loaded));
            }

            printf("[shared_menu] %s config loaded successfully (volume=%u)\n",
                   desc->app_title ? desc->app_title : "App", (unsigned)g_dmg_config.volume_level);

            /* Apply loaded settings to hardware/core */
            if (desc->apply_scale)      desc->apply_scale((dmg_scale_mode_t)g_dmg_config.scale_mode);
            if (desc->apply_effect)     desc->apply_effect(g_dmg_config.grid_effect != 0);
            if (desc->apply_interleave) desc->apply_interleave(g_dmg_config.interleave != 0);
            if (desc->apply_speed)      desc->apply_speed(g_dmg_config.turbo_mode != 0);
            if (desc->apply_volume)     desc->apply_volume((uint8_t)g_dmg_config.volume_level);
            if (desc->apply_brightness) desc->apply_brightness((uint8_t)g_dmg_config.brightness_level);
            if (desc->apply_filter)     desc->apply_filter(g_dmg_config.extra_opt2 == 0u);
            return;
        }

        /* Not a rejection path any more: say why, so a magic or size mismatch
         * is visible in the log instead of silently falling back to defaults
         * forever. The PicoSMS card files carry magic 0x534D5350 while the
         * build expects 0x47475350, which is exactly this case -- it looked
         * like "settings do not persist" rather than "settings are not being
         * read at all". */
        if (fr != FR_OK) {
            printf("[shared_menu] %s config read error %d\n",
                   desc->app_title ? desc->app_title : "App", (int)fr);
        } else if (loaded.magic != desc->config_magic) {
            printf("[shared_menu] %s config magic 0x%08X, expected 0x%08X "
                   "-- ignoring file, using defaults\n",
                   desc->app_title ? desc->app_title : "App",
                   (unsigned)loaded.magic, (unsigned)desc->config_magic);
        } else {
            printf("[shared_menu] %s config only %u bytes (need %u) -- ignoring file\n",
                   desc->app_title ? desc->app_title : "App",
                   (unsigned)br, (unsigned)sizeof(loaded));
        }
    }

    /* Fallback default initialization */
    g_dmg_config.magic = desc->config_magic;
    if (s_global_volume_seen) g_dmg_config.volume_level = s_global_volume;
    if (desc->apply_scale)      desc->apply_scale((dmg_scale_mode_t)g_dmg_config.scale_mode);
    if (desc->apply_volume)     desc->apply_volume((uint8_t)g_dmg_config.volume_level);
    if (desc->apply_brightness) desc->apply_brightness((uint8_t)g_dmg_config.brightness_level);
    if (desc->apply_filter)     desc->apply_filter(true);
}

void dmg_config_save(const dmg_emulator_desc_t *desc) {
    if (desc == NULL || desc->config_path == NULL) return;

    /* Ensure directory exists */
    if (desc->saves_dir) {
        f_mkdir(desc->saves_dir);
    }

    g_dmg_config.magic = desc->config_magic;
    g_dmg_config.version = 1;

    FRESULT fr = f_open(&s_menu_fil, desc->config_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK && desc->config_fallback) {
        fr = f_open(&s_menu_fil, desc->config_fallback, FA_WRITE | FA_CREATE_ALWAYS);
    }

    if (fr == FR_OK) {
        UINT bw = 0;
        f_write(&s_menu_fil, &g_dmg_config, sizeof(g_dmg_config), &bw);
        f_close(&s_menu_fil);
        printf("[shared_menu] %s config saved successfully\n", desc->app_title ? desc->app_title : "App");
    } else {
        printf("[shared_menu] Error saving config: %d\n", fr);
    }
}

/* =========================================================================
 * Path Helpers
 * ========================================================================= */
void dmg_sanitize_basename(const char *game_name, char *out, size_t out_len) {
    if (game_name == NULL || out == NULL || out_len == 0) return;
    const char *slash = strrchr(game_name, '/');
    const char *src = slash ? (slash + 1) : game_name;
    strncpy(out, src, out_len - 1);
    out[out_len - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    for (char *p = out; *p != '\0'; p++) {
        if (*p == ' ' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            *p = '_';
        }
    }
}

void dmg_build_state_path(const char *saves_dir, const char *game_name, uint8_t slot, char *out, size_t out_len) {
    if (out == NULL || out_len == 0) return;

    char base[64];
    dmg_sanitize_basename(game_name, base, sizeof(base));
    const char *dir = saves_dir ? saves_dir : "0:/saves";

    /* Assembled in a buffer of its own before being copied out, because the
     * alternative is the bug DMG_STATE_PATH_MAX documents: snprintf into a
     * caller's undersized buffer drops the last character of "_1.ss" and the
     * state file lands on the card as "_1.s". A name that is one character
     * short is a name nothing else computes.
     *
     * The copy is still bounded -- a caller cannot ask for a bigger path than it
     * can hold -- but the shortfall is now announced, with the intended name in
     * the log, instead of being a silent on-disk artefact. */
    char full[192];
    /* Slot 0 is the standby-resume state (DMG_RESUME_SLOT): "_R.ss", a name
     * no 1..4 slot can produce, so the Load State row never offers it. */
    const int n = (slot == DMG_RESUME_SLOT)
                ? snprintf(full, sizeof(full), "%s/%s_R.ss", dir, base)
                : snprintf(full, sizeof(full), "%s/%s_%u.ss", dir, base, (unsigned)slot);

    if (n < 0 || (size_t)n >= sizeof(full)) {
        printf("[shared_menu] state path does not fit for '%s' slot %u -- nothing written\n",
               base, (unsigned)slot);
        out[0] = '\0';
        return;
    }

    if ((size_t)n >= out_len) {
        printf("[shared_menu] state path buffer too small (%u bytes, needs %u): "
               "would write '%s' -- use DMG_STATE_PATH_MAX\n",
               (unsigned)out_len, (unsigned)(n + 1u), full);
    }

    snprintf(out, out_len, "%s", full);
}

/* Is there a state file at this path that is worth offering to load?
 *
 * Zero length counts as absent, and there is a real one of those on the cards:
 * I:\saves\md\Sonic_The_Hedgehog_(USA,_Europe)_2.ss is 0 bytes, left by a save
 * that was cut short. Offering it would turn a slot the player can see into a
 * load that fails, or worse, into a partial restore. "Exists" is not the same
 * question as "can be loaded from", and the menu needs the second one.
 *
 * f_open rather than f_stat: the file has to be opened to learn its size
 * anyway, and this needs no FF_FS_STAT, which some of the FatFs copies in this
 * project do not enable. */
bool dmg_state_file_present(const char *path) {
    if (path == NULL || path[0] == '\0') return false;

    if (f_open(&s_menu_fil, path, FA_READ) != FR_OK) return false;

    const bool usable = (f_size(&s_menu_fil) > 0);
    f_close(&s_menu_fil);
    return usable;
}

/* Slot probe used by the in-game menu: which of slots 1..4 can be loaded.
 *
 * The generic branch is deliberately narrow: ONE path, <saves_dir>/<game>_<slot>.ss,
 * which is exactly what seven of the nine load_state() implementations will open
 * and nothing else. A wider probe would offer slots that fail to load -- the row
 * would say a state is there, `A` would answer "Empty Load State" -- so an
 * emulator whose loader also reads a fallback directory supplies
 * desc->state_exists instead of being papered over here. */
bool dmg_state_slot_exists(const dmg_emulator_desc_t *desc, const char *game_name, uint8_t slot) {
    if (desc == NULL || slot < 1u || slot > 4u) return false;
    if (desc->state_exists) return desc->state_exists(game_name, slot);
    if (desc->saves_dir == NULL) return false;

    char path[DMG_STATE_PATH_MAX];
    dmg_build_state_path(desc->saves_dir, game_name, slot, path, sizeof(path));
    return dmg_state_file_present(path);
}

/* =========================================================================
 * UI Drawing Bars & Screen Helpers
 * ========================================================================= */
static void fill_rect_safe(const dmg_emulator_desc_t *desc, int x, int y, int w, int h, uint16_t color) {
    if (desc && desc->display.fill_rect) {
        desc->display.fill_rect(x, y, w, h, color);
    }
}

/* Margin kept clear on BOTH sides of every piece of text drawn inside the active
 * window. One constant, so the list rows, the bars, the game name and the status
 * message all line up on the same left edge and honour the same right edge. */
#define DMG_UI_TEXT_MARGIN 12u

/* Glyph width. The font is fixed pitch (mk_ili9225_text advances x by 8 per
 * character), which is what makes exact clipping possible here. */
#define DMG_GLYPH_PX 8u

/* Width of the drawable frame inside the active window: the active window minus
 * the two margins.
 *
 * Rounded DOWN to a whole number of glyphs. That is the whole trick behind the
 * clipping: a glyph is only ever drawn when it lies entirely inside the frame,
 * so no glyph is ever cut. Cutting one is not an option, because mk_ili9225_blit
 * does not clip -- it clamps the width and then streams that many pixels out of a
 * row-major 8x8 buffer, which skews the glyph instead of trimming it. An earlier
 * version tried to hide the overspill by re-blacking the margins afterwards, which
 * is what ate the end of the text.
 *
 * The cost is that 266 - 24 = 242 is not a multiple of 8, so the usable width is
 * 240 and the real right margin is 14px rather than 12. Using a 13px margin would
 * make GB_MODE=1 exact but leave 320 - 26 = 294 for the full-screen mode, which is
 * not a multiple of 8 either -- no margin makes both modes land on a glyph
 * boundary. A 2px margin is far less visible than a sliced glyph.
 *
 * Public: screens that cannot route their text through draw_text_windowed still
 * need to measure the same frame instead of hardcoding x against a flat 320x240,
 * which is how the Lynx loading screen used to push glyphs into the DMG bezel. */
uint16_t dmg_text_frame_w(uint16_t ww)
{
    const uint16_t raw = (uint16_t)(ww - 2u * DMG_UI_TEXT_MARGIN);
    return (uint16_t)((raw / DMG_GLYPH_PX) * DMG_GLYPH_PX);
}

/* forward: the helper below calls text_safe, which is defined after it */
static void text_safe(const dmg_emulator_desc_t *desc, const char *text, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg);

/* Draw one line of text clipped to the frame the margins leave.
 *
 * `centre` centres the line inside the frame; otherwise it starts at the left
 * margin. Text wider than the frame is not truncated -- scroll_px, which the
 * caller animates, slides it left so the rest can be read.
 *
 * Clipping is "draw a glyph only if it fits whole". That is stricter than a
 * rectangle test on each pixel and is what guarantees the result: nothing is ever
 * drawn on or past either margin, at any scroll offset, so no corrective erasing
 * is needed and no glyph can be sliced. Glyphs leave on the left and arrive on the
 * right whole, which is what a marquee should look like.
 *
 * Glyphs are emitted one at a time because draw_text takes no clip argument, and
 * because at full scroll the line origin is left of the window: as an unsigned
 * coordinate that wraps to ~65000 and the text leaves the panel entirely, which
 * is what made a long name vanish. The origin below is signed. */
static void draw_text_windowed(const dmg_emulator_desc_t *desc,
                               const char *text, uint16_t y,
                               uint16_t fg, uint16_t bg,
                               bool centre, uint16_t scroll_px)
{
    if (!desc || !text || text[0] == '\0') return;

    const uint16_t wx = dmg_get_window_x();
    const uint16_t ww = dmg_get_window_w();
    const uint16_t frame_w = dmg_text_frame_w(ww);
    const size_t len = strlen(text);
    const uint16_t text_px = (uint16_t)(len * DMG_GLYPH_PX);

    /* The frame: the only pixels text may touch. */
    const int16_t frame_x0 = (int16_t)(wx + DMG_UI_TEXT_MARGIN);
    const int16_t frame_x1 = (int16_t)(wx + DMG_UI_TEXT_MARGIN + frame_w);

    int16_t x0;
    uint16_t scroll_max = 0;

    if (text_px <= frame_w) {
        /* Fits. Centre on the glyph grid so both margins come out equal; the
         * leftover pixels stay on the left, which is invisible at 8px pitch. */
        const uint16_t slack_glyphs = (uint16_t)((frame_w - text_px) / DMG_GLYPH_PX);
        x0 = frame_x0;
        if (centre)
            x0 = (int16_t)(x0 + (int16_t)((slack_glyphs / 2u) * DMG_GLYPH_PX));
    } else {
        /* Scroll. The range ends with the LAST glyph fully inside the frame, so
         * the tail of the name is actually readable rather than sitting half past
         * the margin. */
        const uint16_t visible_glyphs = (uint16_t)(frame_w / DMG_GLYPH_PX);
        scroll_max = (uint16_t)(((uint32_t)len - visible_glyphs) * DMG_GLYPH_PX);
        if (scroll_px > scroll_max) scroll_px = scroll_max;
        x0 = (int16_t)(frame_x0 - (int16_t)scroll_px);
    }

    for (size_t i = 0; i < len; i++) {
        const int16_t cx = (int16_t)(x0 + (int16_t)(i * DMG_GLYPH_PX));

        /* Whole-glyph containment: the only test, and the only clip there is. */
        if (cx < frame_x0) continue;
        if (cx + (int16_t)DMG_GLYPH_PX > frame_x1) break;

        const char ch[2] = { text[i], '\0' };
        text_safe(desc, ch, (uint16_t)cx, y, fg, bg);
    }
}
static void text_safe(const dmg_emulator_desc_t *desc, const char *text, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg) {
    if (desc && desc->display.draw_text) {
        desc->display.draw_text(text, x, y, fg, bg);
    }
}

/* =========================================================================
 * Word-wrapped text
 * =========================================================================
 * Two passes: lay the words out into fixed line buffers first, then draw. Doing
 * it in one pass made the over-long-word case impossible to reason about -- the
 * split consumed part of a word without advancing the cursor, so the remainder
 * was redrawn on the next line. Laying out first also means the "does it need
 * more lines than we are allowed to draw" question is answered before anything
 * is rendered, so the ellipsis is placed correctly the first time.
 *
 * Two rules keep glyphs from ever crossing the frame:
 *
 *   1. A line only receives whole words, so its pixel width is at most
 *      max_cols * DMG_GLYPH_PX <= frame_w. Nothing has to be clipped afterwards.
 *   2. A single word wider than the whole frame is hard-split at max_cols.
 *      Refusing to split it would put it on a line that still overflows, which
 *      is the exact failure this function exists to prevent.
 *
 * When the text needs more lines than max_lines, the final drawn line is
 * shortened to make room for "...". A silent cut is not acceptable here: unlike
 * the marquee in the ROM list there is no second chance to reveal the tail, so
 * a cut that looks like clipping would read as a rendering bug.
 */
#define DMG_WRAP_MAX_LINES 4u   /* layout storage; callers use 2 */
#define DMG_WRAP_MAX_COLS  40u  /* widest frame today is 296 px -> 37 cols */

void dmg_draw_wrapped_text(const dmg_emulator_desc_t *desc, const char *text,
                           int16_t x, int16_t y,
                           uint16_t fg, uint16_t bg,
                           uint16_t frame_w, uint8_t line_pitch_px, uint8_t max_lines)
{
    if (!desc || !desc->display.draw_text || !text || text[0] == '\0') return;
    if (max_lines == 0u) return;
    if (line_pitch_px < DMG_GLYPH_PX) line_pitch_px = DMG_GLYPH_PX;

    uint16_t max_cols = (uint16_t)(frame_w / DMG_GLYPH_PX);
    if (max_cols == 0u) return;
    if (max_cols > DMG_WRAP_MAX_COLS) max_cols = DMG_WRAP_MAX_COLS;
    if (max_lines > DMG_WRAP_MAX_LINES) max_lines = DMG_WRAP_MAX_LINES;

    /* Bounded working copy: a pathological input must not walk off the stack. */
    char work[128];
    uint16_t len = 0u;
    while (text[len] != '\0' && len < (uint16_t)(sizeof(work) - 1u)) {
        work[len] = text[len];
        len++;
    }
    work[len] = '\0';

    /* ---- pass 1: lay out ---- */
    static char lines[DMG_WRAP_MAX_LINES][DMG_WRAP_MAX_COLS + 1u];
    uint8_t nlines = 0u;
    uint16_t pos = 0u;

    while (pos < len && nlines < DMG_WRAP_MAX_LINES) {
        char *dst = lines[nlines];
        uint16_t d = 0u;

        /* Whole words, while they still fit. */
        while (pos < len) {
            uint16_t wlen = 0u;
            while ((uint16_t)(pos + wlen) < len && work[pos + wlen] != ' ') {
                wlen++;
            }
            const uint16_t need = (uint16_t)((d > 0u ? 1u : 0u) + wlen);

            if (d > 0u && (uint16_t)(d + need) > max_cols) {
                break;                       /* would overflow: new line */
            }
            if (d == 0u && wlen > max_cols) {
                /* Wider than the whole frame: hard-split. */
                for (uint16_t i = 0u; i < max_cols; i++) {
                    dst[d++] = work[pos + i];
                }
                pos = (uint16_t)(pos + max_cols);
                break;
            }
            if (d > 0u) dst[d++] = ' ';
            for (uint16_t i = 0u; i < wlen; i++) {
                dst[d++] = work[pos + i];
            }
            pos = (uint16_t)(pos + wlen);
            while (pos < len && work[pos] == ' ') {
                pos++;                       /* collapse the separator */
            }
        }

        if (d == 0u) break;                  /* no progress: avoid looping */
        dst[d] = '\0';
        nlines++;
    }

    const bool overflowing = (pos < len) || (nlines > max_lines);

    /* ---- pass 2: draw, with an ellipsis if the text did not fit ---- */
    for (uint8_t i = 0u; i < max_lines; i++) {
        if (i >= nlines) break;

        if (overflowing && i == (uint8_t)(max_lines - 1u)) {
            /* Keep the leading text, reserve 3 columns for the ellipsis. */
            uint16_t keep = (uint16_t)(max_cols - 3u);
            if (keep > DMG_WRAP_MAX_COLS - 3u) keep = DMG_WRAP_MAX_COLS - 3u;
            memcpy(lines[i], lines[i], keep);
            lines[i][keep] = '\0';
            strncat(lines[i], "...", (size_t)(sizeof(lines[i]) - strlen(lines[i]) - 1u));
        }

        text_safe(desc, lines[i], (uint16_t)x,
                  (uint16_t)((int16_t)y + (int16_t)((uint16_t)i * line_pitch_px)),
                  fg, bg);
    }
}

static void flush_rows_safe(const dmg_emulator_desc_t *desc, int y_start, int y_end) {
    if (desc && desc->display.flush_rows) {
        desc->display.flush_rows(y_start, y_end);
    }
}

static void flush_safe(const dmg_emulator_desc_t *desc, bool wait) {
    if (desc && desc->display.flush) {
        desc->display.flush(wait);
    }
}

static void dmg_clear_side_borders(const dmg_emulator_desc_t *desc) {
    if (dmg_is_gb_mode()) {
        fill_rect_safe(desc, 0, 0, 45, 240, 0x0000);
        fill_rect_safe(desc, 311, 0, 9, 240, 0x0000);
    }
}

static void fill_screen_safe(const dmg_emulator_desc_t *desc, uint16_t color) {
    if (dmg_is_gb_mode()) {
        dmg_clear_side_borders(desc);
        fill_rect_safe(desc, DMG_GB_WINDOW_X, 0, DMG_GB_WINDOW_W, 240, color);
    } else {
        if (desc && desc->display.fill_screen) {
            desc->display.fill_screen(color);
        } else {
            fill_rect_safe(desc, 0, 0, 320, 240, color);
        }
    }
}

/* =========================================================================
 * Global Volume Management & OSD HUD Overlay
 * ========================================================================= */
static uint32_t s_hud_expire_time_ms = 0;
static uint8_t  s_hud_volume_level = 3;
static bool     s_hud_active = false;
static uint16_t s_hud_cache[20][160];
static int      s_hud_w = 160;
static int      s_hud_x = 80;

static const uint8_t s_hud_font[][8] = {
    /* ' ' (0) */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* '[' (1) */ { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00 },
    /* ']' (2) */ { 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x78, 0x00 },
    /* 'E' (3) */ { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 },
    /* 'L' (4) */ { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 },
    /* 'M' (5) */ { 0x66, 0x7E, 0x5A, 0x42, 0x42, 0x42, 0x42, 0x00 },
    /* 'O' (6) */ { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
    /* 'T' (7) */ { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
    /* 'U' (8) */ { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
    /* 'V' (9) */ { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },
};

static const uint8_t *hud_get_glyph(char c) {
    switch (c) {
        case ' ': return s_hud_font[0];
        case '[': return s_hud_font[1];
        case ']': return s_hud_font[2];
        case 'E': return s_hud_font[3];
        case 'L': return s_hud_font[4];
        case 'M': return s_hud_font[5];
        case 'O': return s_hud_font[6];
        case 'T': return s_hud_font[7];
        case 'U': return s_hud_font[8];
        case 'V': return s_hud_font[9];
        default:  return s_hud_font[0];
    }
}

static void hud_draw_char(char c, int tx, int ty, uint16_t col) {
    const uint8_t *glyph = hud_get_glyph(c);
    for (int r = 0; r < 8; r++) {
        int py = ty + r;
        if (py < 0 || py >= 20) continue;
        uint8_t bits = glyph[r];
        for (int b = 0; b < 8; b++) {
            int px = tx + b;
            if (px < 0 || px >= 160) continue;
            if (bits & (0x80 >> b)) {
                s_hud_cache[py][px] = col;
            }
        }
    }
}

static void hud_draw_string(const char *str, int tx, int ty, uint16_t col) {
    while (*str) {
        hud_draw_char(*str, tx, ty, col);
        tx += 8;
        str++;
    }
}

static void hud_fill_rect(int rx, int ry, int rw, int rh, uint16_t col) {
    for (int y = ry; y < ry + rh; y++) {
        if (y < 0 || y >= 20) continue;
        for (int x = rx; x < rx + rw; x++) {
            if (x < 0 || x >= 160) continue;
            s_hud_cache[y][x] = col;
        }
    }
}

static inline uint16_t rgb565_to_rgb555_fast(uint16_t c) {
    return (uint16_t)(((c & 0xF800u) >> 1) | ((c & 0x07C0u) >> 1) | (c & 0x001Fu));
}

void dmg_volume_hud_trigger(uint8_t volume_level) {
    s_hud_volume_level = volume_level > 5 ? 5 : volume_level;
    s_hud_expire_time_ms = to_ms_since_boot(get_absolute_time()) + 1000u;
    s_hud_active = true;

    bool gb = dmg_is_gb_mode();
    s_hud_w = gb ? 134 : 160;
    s_hud_x = gb ? 111 : 80;

    int w = s_hud_w;
    int h = 20;

    /* Initialize all pixels to transparent key (0x0001) */
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 160; x++) {
            s_hud_cache[y][x] = 0x0001;
        }
    }

    uint16_t col_border = 0x0000; /* Black border */
    uint16_t col_bg     = 0xEF7D; /* Clean Light UI gray (#EEEEEE) in RGB565 -- 0xEEEE is the RGB444 value and came out salmon */
    uint16_t col_text   = 0x2104; /* Charcoal (#222222) */
    uint16_t col_fill   = 0x3666; /* Mint Green (#33CC66) */
    uint16_t col_off    = 0x4208; /* Dark Slate Gray (#444444) */
    uint16_t col_err    = 0xF800; /* Pure Red (#FF0000) */

    /* Rounded border (radius 3) */
    hud_fill_rect(3, 0, w - 6, 1, col_border);
    hud_fill_rect(1, 1, w - 2, 1, col_border);
    hud_fill_rect(0, 2, w, h - 4, col_border);
    hud_fill_rect(1, h - 2, w - 2, 1, col_border);
    hud_fill_rect(3, h - 1, w - 6, 1, col_border);

    /* Rounded inner background */
    hud_fill_rect(3, 1, w - 6, 1, col_bg);
    hud_fill_rect(2, 2, w - 4, 1, col_bg);
    hud_fill_rect(1, 3, w - 2, h - 6, col_bg);
    hud_fill_rect(2, h - 3, w - 4, 1, col_bg);
    hud_fill_rect(3, h - 2, w - 6, 1, col_bg);

    if (s_hud_volume_level == 0) {
        /* Level 0: [ MUTE ] banner */
        int txt_x = (w - 64) / 2;
        hud_draw_string("[ MUTE ]", txt_x, 6, col_err);
    } else {
        /* Levels 1..5: "VOL" label + 5 level blocks */
        hud_draw_string("VOL", 8, 6, col_text);

        int blocks_x = 38;
        int total_bw = (w - 46);
        int block_gap = 3;
        int block_w = (total_bw - (4 * block_gap)) / 5;
        int block_h = 10;
        int block_y = 5;

        for (int i = 0; i < 5; i++) {
            int bx = blocks_x + i * (block_w + block_gap);
            uint16_t bcol = (i < s_hud_volume_level) ? col_fill : col_off;
            hud_fill_rect(bx, block_y, block_w, block_h, bcol);
            /* Block border */
            hud_fill_rect(bx, block_y, block_w, 1, col_border);
            hud_fill_rect(bx, block_y + block_h - 1, block_w, 1, col_border);
            hud_fill_rect(bx, block_y, 1, block_h, col_border);
            hud_fill_rect(bx + block_w - 1, block_y, 1, block_h, col_border);
        }
    }
}

bool dmg_volume_hud_is_active(void) {
    if (!s_hud_active) return false;
    if (to_ms_since_boot(get_absolute_time()) >= s_hud_expire_time_ms) {
        s_hud_active = false;
        return false;
    }
    return true;
}

const uint16_t *dmg_volume_hud_get_row(uint16_t screen_row, int *out_x, int *out_w) {
    if (!dmg_volume_hud_is_active()) return NULL;
    if (screen_row < 180 || screen_row >= 200) return NULL;
    if (out_x) *out_x = s_hud_x;
    if (out_w) *out_w = s_hud_w;
    return s_hud_cache[screen_row - 180];
}

void dmg_set_global_volume(uint8_t vol, const dmg_emulator_desc_t *desc) {
    if (vol > 5) vol = 5;
    if (s_global_volume != vol) {
        s_global_volume = vol;
        s_global_volume_dirty = true;
    }
    g_dmg_config.volume_level = vol;
    if (desc && desc->apply_volume) {
        desc->apply_volume(vol);
    }
    dmg_volume_hud_trigger(vol);
    /* Saving to boot.txt deferred to menu exit / commit, NEVER during active gameplay */
}

void dmg_step_global_volume(int delta, const dmg_emulator_desc_t *desc) {
    dmg_init_boot_mode();
    int cur = (int)s_global_volume + delta;
    if (cur < 0) cur = 0;
    if (cur > 5) cur = 5;
    dmg_set_global_volume((uint8_t)cur, desc);
}

void dmg_flush_global_volume(void) {
    if (s_global_volume_dirty) {
        dmg_save_global_volume_file(s_global_volume);
        s_global_volume_dirty = false;
    }
}

bool dmg_process_volume_combo(bool btn_select, bool btn_up, bool btn_down, const dmg_emulator_desc_t *desc) {
    static bool s_last_vup = false;
    static bool s_last_vdown = false;
    bool active = false;

    if (btn_select) {
        if (btn_up) {
            active = true;
            if (!s_last_vup) {
                dmg_step_global_volume(+1, desc);
            }
        }
        if (btn_down) {
            active = true;
            if (!s_last_vdown) {
                dmg_step_global_volume(-1, desc);
            }
        }
    }
    s_last_vup = btn_select && btn_up;
    s_last_vdown = btn_select && btn_down;
    return active;
}

void dmg_volume_hud_render(const dmg_emulator_desc_t *desc) {
    if (desc == NULL || !dmg_volume_hud_is_active()) return;

    /* Render HUD onto the framebuffer if desc provides fill_rect */
    int w = s_hud_w;
    int x0 = s_hud_x;
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < w; c++) {
            uint16_t col = s_hud_cache[r][c];
            if (col != 0x0001) {
                uint16_t pixel = desc->is_rgb555 ? rgb565_to_rgb555_fast(col) : col;
                fill_rect_safe(desc, x0 + c, 180 + r, 1, 1, pixel);
            }
        }
    }
    /* NO full-screen flush or flush_rows here! */
}

/* =========================================================================
 * Battery: shared state, menu indicator, cutoff and in-game overlay
 *
 * One state, sampled every DMG_BATT_PERIOD_MS by whichever loop is running:
 * the game list and the pause menu (battery_tick) or the game itself
 * (dmg_battery_game_tick, from dmg_combo_frame on core 0).
 *
 * Cutoff: DMG_BATTERY_CUTOFF_MV on three samples in a row (~6 s, so a load
 * spike does not turn the console off) and no charger. In a game it only
 * raises request_exit_game -- the emulator leaves through its normal exit,
 * which writes the battery save -- and the warning + standby happen in the
 * menu it returns to, where the screen can be repainted on wake.
 * ========================================================================= */
#define DMG_BATT_ICON_W     18u   /* body, outline included */
#define DMG_BATT_ICON_H     10u
#define DMG_BATT_NUB_W       2u
#define DMG_BATT_NUB_H       4u
#define DMG_BATT_GAP         4u   /* between "87%" and the icon */
/* "100%" + gap + body + nub, rounded up to whole glyphs so the subtitle that is
 * right aligned before it stays on the glyph grid. */
#define DMG_BATT_RESERVE_PX  56u
#define DMG_BATT_PERIOD_MS 2000u
#define DMG_BATT_PULSE_MS   500u
#define DMG_BATT_LOW_SAMPLES  3u

/* Dark green for the charging icon: pure green (#00FF00, pal.status_ok) is
 * unreadable on the light grey bar. #1E8C3A. */
#define DMG_BATT_GREEN_565 0x1C67u
#define DMG_BATT_GREEN_555 0x0E27u

typedef struct {
    bool     present;   /* sensing configured and a cell on the divider */
    uint16_t mv;
    uint8_t  pct;
    bool     usb;       /* charger connected */
    bool     charging;  /* usb and below DMG_BATTERY_FULL_MV */
    bool     full;      /* usb and at/above it */
} batt_state_t;

static batt_state_t s_bs;
static bool         s_bs_valid = false;
static uint32_t     s_bs_next_ms = 0;
static uint8_t      s_low_count = 0;
static bool         s_relock = false;          /* woke from battery standby w/o charger */
static volatile bool s_cutoff_pending = false; /* confirmed in game, menu finishes it */

static batt_state_t s_shown;                   /* what the top bar currently shows */
static bool         s_shown_valid = false;
static bool         s_bolt_on = true;
static uint32_t     s_pulse_next_ms = 0;
static char         s_tb_title[32];
static char         s_tb_subtitle[32];

static void battery_shutdown(const dmg_emulator_desc_t *desc);   /* below confirm_dialog */

static void battery_sample(void) {
    dmg_battery_init_pin(dmg_panel_get_batt_pin());
    dmg_battery_init_charge_pin(dmg_panel_get_charge_pin());

    batt_state_t n;
    memset(&n, 0, sizeof(n));
    if (dmg_battery_configured()) {
        n.mv = dmg_battery_read_mv();
        n.present = (n.mv >= DMG_BATTERY_PRESENT_MV);
        if (n.present) n.pct = dmg_battery_percent(n.mv);
    }
    n.usb = dmg_battery_usb_present();
    n.charging = n.present && n.usb && n.mv < DMG_BATTERY_FULL_MV;
    n.full     = n.present && n.usb && n.mv >= DMG_BATTERY_FULL_MV;

    if (!s_bs_valid || n.pct != s_bs.pct || n.usb != s_bs.usb || n.present != s_bs.present) {
        printf("[battery] %u mV -> %u%%%s\n", (unsigned)n.mv, (unsigned)n.pct,
               n.usb ? (n.full ? " (charger, full)" : " (charging)") : "");
    }
    s_bs = n;
    s_bs_valid = true;
}

/* True when the low-battery standby must happen now. */
static bool battery_cutoff_due(void) {
    if (!s_bs.present || s_bs.usb) {
        s_low_count = 0;
        s_relock = false;
        return false;
    }
    if (s_relock) {
        /* Just woke without a charger: stay off until the cell has recovered
         * a little, instead of running for 6 s and dropping again. */
        if (s_bs.mv < DMG_BATTERY_RELEASE_MV) return true;
        s_relock = false;
    }
    if (s_bs.mv <= DMG_BATTERY_CUTOFF_MV) {
        if (++s_low_count >= DMG_BATT_LOW_SAMPLES) return true;
    } else {
        s_low_count = 0;
    }
    return false;
}

static bool battery_charger_cb(void) {
    return dmg_battery_usb_present();
}

/* ---- in-game overlay (core 0 renders, core 1 stamps) ------------------- */
#define DMG_BOV_W   64
#define DMG_BOV_H   14
#define DMG_BOV_Y0   3
#define DMG_BOV_RED   0xF800u   /* RGB565, panel format */
#define DMG_BOV_BG    0x0000u
#define DMG_BOV_CLEAR 0x0001u   /* transparent key, same as the volume HUD */

static uint16_t          s_bov[2][DMG_BOV_H][DMG_BOV_W];
static volatile uint8_t  s_bov_idx = 0;
static volatile bool     s_bov_visible = false;
static volatile uint32_t s_bov_gen = 0;
static volatile int      s_bov_x = 0;
static uint8_t           s_bov_pct = 0xFF;

static const uint8_t s_bov_digits[11][8] = {
    { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 }, /* 0 */
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, /* 1 */
    { 0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00 }, /* 2 */
    { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 }, /* 3 */
    { 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00 }, /* 4 */
    { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 }, /* 5 */
    { 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 }, /* 6 */
    { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 }, /* 7 */
    { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 }, /* 8 */
    { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 }, /* 9 */
    { 0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0x00 }, /* % */
};

static void bov_rect(uint16_t (*buf)[DMG_BOV_W], int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= DMG_BOV_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= DMG_BOV_W) continue;
            buf[yy][xx] = c;
        }
    }
}

static void bov_glyph(uint16_t (*buf)[DMG_BOV_W], const uint8_t *g, int x, int y, uint16_t c) {
    for (int r = 0; r < 8; r++) {
        for (int b = 0; b < 8; b++) {
            if (g[r] & (0x80u >> b)) bov_rect(buf, x + b, y + r, 1, 1, c);
        }
    }
}

/* Right edge (exclusive) of the indicator in the top bar: the right edge of the
 * text frame, so the overlay sits exactly where the menus draw the marker. */
static int battery_right_edge(void) {
    return (int)(dmg_get_window_x() + DMG_UI_TEXT_MARGIN + dmg_text_frame_w(dmg_get_window_w()));
}

static void battery_overlay_update(void) {
    const bool want = s_bs.present && !s_bs.usb && s_bs.pct <= DMG_BATTERY_LOW_PCT;
    if (!want) {
        if (s_bov_visible) {
            s_bov_visible = false;
            s_bov_gen++;
        }
        s_bov_pct = 0xFF;
        return;
    }
    if (s_bov_visible && s_bov_pct == s_bs.pct) return;

    const uint8_t next = (uint8_t)(s_bov_idx ^ 1u);
    uint16_t (*buf)[DMG_BOV_W] = s_bov[next];

    /* No card: only "NN%" and the icon, in red over the game. A 1 px black
     * outline hugs the glyphs (not a box), so the red stays readable on red or
     * bright scenes; everything else is transparent. */
    bov_rect(buf, 0, 0, DMG_BOV_W, DMG_BOV_H, DMG_BOV_CLEAR);

    /* "NN%" right aligned before the icon */
    char txt[6];
    snprintf(txt, sizeof(txt), "%u%%", (unsigned)s_bs.pct);
    const int icon_x = DMG_BOV_W - 2 - (int)(DMG_BATT_ICON_W + DMG_BATT_NUB_W);
    int tx = icon_x - (int)DMG_BATT_GAP - (int)(strlen(txt) * 8u);
    for (const char *c = txt; *c; c++, tx += 8) {
        const uint8_t *g = (*c == '%') ? s_bov_digits[10]
                         : (*c >= '0' && *c <= '9') ? s_bov_digits[*c - '0'] : NULL;
        if (g) bov_glyph(buf, g, tx, 3, DMG_BOV_RED);
    }

    /* icon */
    const int iy = (DMG_BOV_H - (int)DMG_BATT_ICON_H) / 2;
    bov_rect(buf, icon_x, iy, DMG_BATT_ICON_W, 1, DMG_BOV_RED);
    bov_rect(buf, icon_x, iy + DMG_BATT_ICON_H - 1, DMG_BATT_ICON_W, 1, DMG_BOV_RED);
    bov_rect(buf, icon_x, iy, 1, DMG_BATT_ICON_H, DMG_BOV_RED);
    bov_rect(buf, icon_x + DMG_BATT_ICON_W - 1, iy, 1, DMG_BATT_ICON_H, DMG_BOV_RED);
    bov_rect(buf, icon_x + DMG_BATT_ICON_W, iy + (DMG_BATT_ICON_H - DMG_BATT_NUB_H) / 2,
             DMG_BATT_NUB_W, DMG_BATT_NUB_H, DMG_BOV_RED);
    /* inside of the body: dark, so the fill level reads against the game */
    bov_rect(buf, icon_x + 1, iy + 1, DMG_BATT_ICON_W - 2, DMG_BATT_ICON_H - 2, DMG_BOV_BG);
    int fw = ((int)(DMG_BATT_ICON_W - 4) * (int)s_bs.pct + 50) / 100;
    if (s_bs.pct > 0 && fw == 0) fw = 1;
    bov_rect(buf, icon_x + 2, iy + 2, fw, DMG_BATT_ICON_H - 4, DMG_BOV_RED);

    /* outline: transparent pixels touching red turn black */
    for (int y = 0; y < DMG_BOV_H; y++) {
        for (int x = 0; x < DMG_BOV_W; x++) {
            if (buf[y][x] != DMG_BOV_CLEAR) continue;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= DMG_BOV_H || xx < 0 || xx >= DMG_BOV_W) continue;
                    if (buf[yy][xx] == DMG_BOV_RED) { edge = true; break; }
                }
            }
            if (edge) buf[y][x] = DMG_BOV_BG;
        }
    }

    /* Publish: the buffer is complete before core 1 can see the new index. */
    __dmb();
    s_bov_x = battery_right_edge() + 1 - DMG_BOV_W;
    s_bov_idx = next;
    s_bov_visible = true;
    s_bov_pct = s_bs.pct;
    s_bov_gen++;
}

bool dmg_batt_overlay_visible(void) {
    return s_bov_visible;
}

uint32_t dmg_batt_overlay_generation(void) {
    return s_bov_gen;
}

const uint16_t *dmg_batt_overlay_get_row(uint16_t screen_y, int *out_x, int *out_w) {
    if (!s_bov_visible) return NULL;
    if (screen_y < DMG_BOV_Y0 || screen_y >= DMG_BOV_Y0 + DMG_BOV_H) return NULL;
    if (out_x) *out_x = s_bov_x;
    if (out_w) *out_w = DMG_BOV_W;
    return s_bov[s_bov_idx][screen_y - DMG_BOV_Y0];
}

uint16_t dmg_batt_overlay_y0(void) { return DMG_BOV_Y0; }
uint16_t dmg_batt_overlay_h(void)  { return DMG_BOV_H; }

void __not_in_flash_func(dmg_batt_overlay_stamp_row)(uint16_t screen_y, uint16_t *buf,
                                                     int buf_x0, int buf_w, bool swap) {
    if (!s_bov_visible || buf == NULL) return;
    if (screen_y < DMG_BOV_Y0 || screen_y >= DMG_BOV_Y0 + DMG_BOV_H) return;
    const uint16_t *src = s_bov[s_bov_idx][screen_y - DMG_BOV_Y0];
    const int ox = s_bov_x;
    for (int i = 0; i < DMG_BOV_W; i++) {
        const uint16_t c = src[i];
        if (c == DMG_BOV_CLEAR) continue;
        const int t = ox + i - buf_x0;
        if (t < 0 || t >= buf_w) continue;
        buf[t] = swap ? __builtin_bswap16(c) : c;
    }
}

bool __not_in_flash_func(dmg_batt_overlay_row_hit)(uint16_t screen_y) {
    return s_bov_visible && screen_y >= DMG_BOV_Y0 && screen_y < DMG_BOV_Y0 + DMG_BOV_H;
}

uint32_t __not_in_flash_func(dmg_batt_overlay_row_salt)(uint16_t screen_y) {
    if (screen_y < DMG_BOV_Y0 || screen_y >= DMG_BOV_Y0 + DMG_BOV_H) return 0u;
    return (s_bov_gen * 0x9E3779B1u) ^ (s_bov_visible ? 0x5A5A5A5Au : 0u);
}

/* Border part of the overlay, i.e. what the game image [gx0,gx1) x [gy0,gy1)
 * does not cover and therefore never rewrites. Pushed over black every frame
 * while shown (a menu or a scale change may have cleared it), and once in black
 * when it hides. Core 1 only. */
static uint16_t s_bov_strip[DMG_BOV_H * DMG_BOV_W];
static bool     s_bov_strip_drawn = false;
static int      s_bov_strip_x = 0;

static void __not_in_flash_func(bov_push_block)(int x0, int x1, int y0, int y1, int ox,
                                                bool visible, dmg_batt_push_fn push) {
    if (x1 <= x0 || y1 <= y0) return;
    const int w = x1 - x0;
    int n = 0;
    for (int y = y0; y < y1; y++) {
        const uint16_t *src = visible ? s_bov[s_bov_idx][y - DMG_BOV_Y0] : NULL;
        for (int x = x0; x < x1; x++) {
            uint16_t c = src ? src[x - ox] : 0x0000u;
            if (c == DMG_BOV_CLEAR) c = 0x0000u;
            s_bov_strip[n++] = c;
        }
    }
    push((uint16_t)x0, (uint16_t)y0, (uint16_t)w, (uint16_t)(y1 - y0), s_bov_strip);
}

/* Queue-based drivers (rows composed on core 0 and skipped when unchanged).
 * The game marks every row it emits; at frame end the overlay rows it did not
 * emit (above/below the image) are returned so the driver queues them as
 * black rows with the overlay stamped. Rows are re-sent when the overlay
 * changes and every 30 frames, so a menu or a screen clear never leaves them
 * blank for long. Interleave emits each row every other frame, so coverage is
 * taken over the last two frames. */
static uint32_t s_bovq_cur = 0, s_bovq_prev = 0;
static uint32_t s_bovq_key[DMG_BOV_H];
static uint8_t  s_bovq_tick = 0;

void __not_in_flash_func(dmg_batt_overlay_mark_row)(uint16_t row) {
    const int i = (int)row - DMG_BOV_Y0;
    if (i >= 0 && i < DMG_BOV_H) s_bovq_cur |= (1u << i);
}

uint32_t __not_in_flash_func(dmg_batt_overlay_frame_rows)(void) {
    const uint32_t covered = s_bovq_cur | s_bovq_prev;
    s_bovq_prev = s_bovq_cur;
    s_bovq_cur = 0;
    const bool refresh = (++s_bovq_tick >= 30u);
    if (refresh) s_bovq_tick = 0;
    const uint32_t key = s_bov_visible ? (s_bov_gen | 0x80000000u) : 0u;
    uint32_t out = 0;
    for (int i = 0; i < DMG_BOV_H; i++) {
        if (covered & (1u << i)) { s_bovq_key[i] = 0xFFFFFFFFu; continue; }
        /* hidden and already black: nothing to do */
        if (key == 0u && s_bovq_key[i] == 0u) continue;
        if (s_bovq_key[i] != key || (refresh && key != 0u)) {
            s_bovq_key[i] = key;
            out |= (1u << i);
        }
    }
    return out;
}

bool __not_in_flash_func(dmg_batt_overlay_borders_pending)(void) {
    return s_bov_visible || s_bov_strip_drawn;
}

void __not_in_flash_func(dmg_batt_overlay_push_borders)(int gx0, int gx1, int gy0, int gy1,
                                                        dmg_batt_push_fn push) {
    if (push == NULL) return;
    const bool visible = s_bov_visible;
    if (!visible && !s_bov_strip_drawn) return;
    const int ox = visible ? s_bov_x : s_bov_strip_x;
    const int ox0 = ox, ox1 = ox + DMG_BOV_W;
    const int oy0 = DMG_BOV_Y0, oy1 = DMG_BOV_Y0 + DMG_BOV_H;

    /* overlay rows that overlap the image rows: [my0, my1) */
    int my0 = gy0 > oy0 ? gy0 : oy0;
    int my1 = gy1 < oy1 ? gy1 : oy1;
    if (my1 < my0) { my0 = oy1; my1 = oy1; }   /* no overlap: all rows are border */
    /* rows above and below the image: full width */
    bov_push_block(ox0, ox1, oy0, my0, ox, visible, push);
    bov_push_block(ox0, ox1, my1, oy1, ox, visible, push);
    /* rows beside the image: left and right of it */
    const int lx1 = ox1 < gx0 ? ox1 : gx0;
    const int rx0 = ox0 > gx1 ? ox0 : gx1;
    bov_push_block(ox0, lx1, my0, my1, ox, visible, push);
    bov_push_block(rx0, ox1, my0, my1, ox, visible, push);

    s_bov_strip_drawn = visible;
    s_bov_strip_x = ox;
}

bool dmg_battery_game_tick(void) {
    if (s_cutoff_pending) return true;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (s_bs_valid && (int32_t)(now - s_bs_next_ms) < 0) return false;
    s_bs_next_ms = now + DMG_BATT_PERIOD_MS;
    battery_sample();
    if (battery_cutoff_due()) {
        printf("[battery] cutoff in game: leaving the game first\n");
        s_cutoff_pending = true;
        if (s_bov_visible) { s_bov_visible = false; s_bov_gen++; }
        return true;
    }
    battery_overlay_update();
    return false;
}

/* ---- menu indicator ----------------------------------------------------- */
static bool batt_shown_differs(void) {
    return !s_shown_valid || s_shown.present != s_bs.present || s_shown.pct != s_bs.pct ||
           s_shown.charging != s_bs.charging || s_shown.full != s_bs.full;
}

/* 6 x 8 lightning bolt, drawn over the icon body. */
static const uint8_t s_bolt[8] = { 0x06, 0x0C, 0x18, 0x3E, 0x0C, 0x18, 0x30, 0x20 };

/* Draws "NN%" and the icon so they end at `right` (exclusive). */
static void draw_battery_indicator(const dmg_emulator_desc_t *desc, const dmg_palette_t *pal,
                                   int right, const batt_state_t *st, bool bolt) {
    const int body_x = right - (int)(DMG_BATT_NUB_W + DMG_BATT_ICON_W);
    const int body_y = (int)((20u - DMG_BATT_ICON_H) / 2u);
    const bool on_usb = st->charging || st->full;
    const uint16_t green = desc->is_rgb555 ? DMG_BATT_GREEN_555 : DMG_BATT_GREEN_565;
    const uint16_t fg = on_usb ? green
                      : (st->pct <= DMG_BATTERY_LOW_PCT) ? pal->sel : pal->text_bar;

    fill_rect_safe(desc, right - (int)DMG_BATT_RESERVE_PX, 0, (int)DMG_BATT_RESERVE_PX, 20, pal->bar);

    /* outline */
    fill_rect_safe(desc, body_x, body_y, DMG_BATT_ICON_W, 1, fg);
    fill_rect_safe(desc, body_x, body_y + DMG_BATT_ICON_H - 1, DMG_BATT_ICON_W, 1, fg);
    fill_rect_safe(desc, body_x, body_y, 1, DMG_BATT_ICON_H, fg);
    fill_rect_safe(desc, body_x + DMG_BATT_ICON_W - 1, body_y, 1, DMG_BATT_ICON_H, fg);
    /* terminal */
    fill_rect_safe(desc, body_x + DMG_BATT_ICON_W, body_y + (DMG_BATT_ICON_H - DMG_BATT_NUB_H) / 2,
                   DMG_BATT_NUB_W, DMG_BATT_NUB_H, fg);
    /* charge, inside a 1 px gap from the outline: 14 x 6 px */
    const int inner_w = (int)DMG_BATT_ICON_W - 4;
    int fw = (inner_w * (int)st->pct + 50) / 100;
    if (st->pct > 0 && fw == 0) fw = 1;
    if (fw > 0) {
        fill_rect_safe(desc, body_x + 2, body_y + 2, fw, (int)DMG_BATT_ICON_H - 4, fg);
    }

    /* Bolt: cut out of the fill (bar colour) where it is filled, drawn in the
     * icon colour where it is empty, so it reads at any charge level. */
    if (on_usb && bolt) {
        const int bx = body_x + (int)(DMG_BATT_ICON_W - 6u) / 2;
        const int fill_end = body_x + 2 + fw;
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 6; c++) {
                if (!(s_bolt[r] & (0x20u >> c))) continue;
                const int px = bx + c, py = body_y + 1 + r;
                const bool in_fill = (px >= body_x + 2 && px < fill_end &&
                                      py >= body_y + 2 && py < body_y + (int)DMG_BATT_ICON_H - 2);
                fill_rect_safe(desc, px, py, 1, 1, in_fill ? pal->bar : fg);
            }
        }
    }

    char txt[8];
    snprintf(txt, sizeof(txt), "%u%%", (unsigned)st->pct);
    const int tx = body_x - (int)DMG_BATT_GAP - (int)(strlen(txt) * DMG_GLYPH_PX);
    text_safe(desc, txt, (uint16_t)tx, 6, fg, pal->bar);
}

static void draw_top_bar_impl(const dmg_emulator_desc_t *desc, const char *title,
                              const char *subtitle, bool battery) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    bool show_batt = false;
    if (battery) {
        snprintf(s_tb_title, sizeof(s_tb_title), "%s", title ? title : "");
        snprintf(s_tb_subtitle, sizeof(s_tb_subtitle), "%s", subtitle ? subtitle : "");
        if (!s_bs_valid) {
            battery_sample();
            s_bs_next_ms = to_ms_since_boot(get_absolute_time()) + DMG_BATT_PERIOD_MS;
        }
        show_batt = s_bs.present;
        s_shown = s_bs;
        s_shown_valid = true;
    }
    const uint16_t reserve = show_batt ? DMG_BATT_RESERVE_PX : 0u;

    dmg_clear_side_borders(desc);

    fill_rect_safe(desc, wx, 0, ww, 20, pal.bar);
    if (title && title[0] != '\0') {
        draw_text_windowed(desc, title, 6, pal.text_bar, pal.bar, false, 0u);
    }

    const uint16_t frame_w = dmg_text_frame_w(ww);
    const int16_t frame_x0 = (int16_t)(wx + DMG_UI_TEXT_MARGIN);

    if (subtitle && subtitle[0] != '\0') {
        /* Right aligned to the frame's right edge (or to the battery indicator
         * when one is shown), so it lines up with the title on the left.
         * draw_text_windowed only clips on the left, so this is positioned
         * rather than routed through it. */
        const uint16_t avail = (uint16_t)(frame_w - reserve);
        const size_t slen = strlen(subtitle);
        const uint16_t spx = (uint16_t)(slen * DMG_GLYPH_PX);
        if (spx <= avail) {
            const uint16_t slack = (uint16_t)((avail - spx) / DMG_GLYPH_PX);
            const int16_t sx = (int16_t)(frame_x0 + (int16_t)(slack * DMG_GLYPH_PX));
            text_safe(desc, subtitle, (uint16_t)sx, 6, pal.text_bar, pal.bar);
        }
    }

    if (show_batt) {
        draw_battery_indicator(desc, &pal, battery_right_edge(), &s_shown, s_bolt_on || s_shown.full);
    }

    /* 1px bottom border line */
    fill_rect_safe(desc, wx, 20, ww, 1, pal.border);
    flush_rows_safe(desc, 0, 20);
}

void dmg_draw_top_bar(const dmg_emulator_desc_t *desc, const char *title, const char *subtitle) {
    draw_top_bar_impl(desc, title, subtitle, false);
}

/* Called from the game list and pause menu loops. Samples every
 * DMG_BATT_PERIOD_MS, pulses the charging bolt every DMG_BATT_PULSE_MS, and runs
 * the low-battery cutoff. Returns true when the screen was lost to a standby and
 * the caller must repaint everything. */
static bool battery_tick(const dmg_emulator_desc_t *desc) {
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    if (s_cutoff_pending) {
        battery_shutdown(desc);
        return true;
    }

    bool full_redraw = false;
    if ((int32_t)(now - s_bs_next_ms) >= 0) {
        s_bs_next_ms = now + DMG_BATT_PERIOD_MS;
        battery_sample();
        if (battery_cutoff_due()) {
            battery_shutdown(desc);
            return true;
        }
        if (batt_shown_differs()) full_redraw = true;
    }

    if (full_redraw) {
        draw_top_bar_impl(desc, s_tb_title, s_tb_subtitle, true);
        return false;
    }

    /* Pulse: only the indicator region, not the whole bar. */
    if (s_shown_valid && s_shown.present && s_shown.charging &&
        (int32_t)(now - s_pulse_next_ms) >= 0) {
        s_pulse_next_ms = now + DMG_BATT_PULSE_MS;
        s_bolt_on = !s_bolt_on;
        dmg_palette_t pal;
        dmg_get_palette(desc, &pal);
        draw_battery_indicator(desc, &pal, battery_right_edge(), &s_shown, s_bolt_on);
        flush_rows_safe(desc, 0, 20);
    }
    return false;
}

void dmg_draw_bottom_bar(const dmg_emulator_desc_t *desc, const char *left_hint, const char *right_hint) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    dmg_clear_side_borders(desc);

    /* 1px top border line */
    fill_rect_safe(desc, wx, 219, ww, 1, pal.border);
    fill_rect_safe(desc, wx, 220, ww, 20, pal.bar);

    if (left_hint && left_hint[0] != '\0') {
        draw_text_windowed(desc, left_hint, 226, pal.text_bar, pal.bar, false, 0u);
    }

    /* Right hint aligned to the frame's right edge, mirroring the title. */
    const char *r_text = (right_hint && right_hint[0] != '\0') ? right_hint : "B: Back";
    {
        const uint16_t frame_w = dmg_text_frame_w(ww);
        const int16_t frame_x0 = (int16_t)(wx + DMG_UI_TEXT_MARGIN);
        const uint16_t rpx = (uint16_t)(strlen(r_text) * DMG_GLYPH_PX);
        if (rpx <= frame_w) {
            const uint16_t slack = (uint16_t)((frame_w - rpx) / DMG_GLYPH_PX);
            const int16_t rx = (int16_t)(frame_x0 + (int16_t)(slack * DMG_GLYPH_PX));
            text_safe(desc, r_text, (uint16_t)rx, 226, pal.text_bar, pal.bar);
        }
    }

    flush_rows_safe(desc, 219, 239);
}

/* Strip directory, extension and underscores: the form the list shows. Returns
 * the resulting length so callers do not have to strlen it again. */
static size_t rom_display_name(const char *filename, char *out, size_t out_len)
{
    const char *slash = strrchr(filename, '/');
    const char *src = slash ? (slash + 1) : filename;

    strncpy(out, src, out_len - 1u);
    out[out_len - 1u] = '\0';

    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';

    for (char *p = out; *p != '\0'; p++) {
        if (*p == '_') *p = ' ';
    }

    return strlen(out);
}
/* Marquee timing for names wider than the active window. */
/* Marquee timing. The name slides back and forth between the two ends rather
 * than wrapping, so reading it never restarts from the left. The step is 2px every
 * 70ms (~29 px/s) -- slow enough for a 36-character name to be followed, which at
 * the earlier 50 px/s it was not. */
#define DMG_ROM_SCROLL_STEP_PX 2u
#define DMG_ROM_SCROLL_STEP_MS 70u
/* Pause at each end before reversing, or the turn is invisible and the motion
 * just reads as a stutter. */
#define DMG_ROM_SCROLL_EDGE_MS 900u


/* Pixels of a displayed name, and how many pixels are actually available
 * between the two margins. */
static uint16_t rom_name_px(const char *filename, char *out, size_t out_len, uint16_t ww)
{
    if (!filename || filename[0] == '\0') {
        if (out_len) out[0] = '\0';
        return 0;
    }

    const size_t len = rom_display_name(filename, out, out_len);

    /* Scroll range, using the same frame width and the same whole-glyph rule as
     * draw_text_windowed. If this used a different frame the marquee would run
     * past the point the drawing code stops at, and the tail would never be
     * brought fully into view. */
    const uint16_t frame_w = dmg_text_frame_w(ww);
    const uint16_t text_px = (uint16_t)(len * DMG_GLYPH_PX);

    if (text_px <= frame_w) return 0;

    return (uint16_t)(text_px - frame_w);
}

static char s_active_game_name[64] = "";
static bool s_status_active = false;

/* Marquee state for the game-name bar. Held in statics so dmg_draw_game_bar's
 * signature -- which all nine emulators call -- does not have to change. */
static uint16_t s_game_bar_scroll = 0;
static uint32_t s_game_bar_ms = 0;
/* -1 sliding towards the left (higher offset), +1 towards the right. The name
 * reverses at each end rather than jumping back to the start. */
static int8_t s_game_bar_dir = 1;

/* Put the name back at the start and send it off to the right. */
static void game_bar_scroll_reset(void)
{
    s_game_bar_scroll = 0;
    s_game_bar_ms = 0;
    s_game_bar_dir = 1;
}

void dmg_draw_game_bar(const dmg_emulator_desc_t *desc, const char *game_name) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    dmg_clear_side_borders(desc);

    fill_rect_safe(desc, wx, 21, ww, 21, pal.bar);
    fill_rect_safe(desc, wx, 42, ww, 1, pal.border);

    if (game_name && game_name[0] != '\0') {
        char disp_name[44];
        rom_display_name(game_name, disp_name, sizeof(disp_name));
        draw_text_windowed(desc, disp_name, 27, pal.text_bar, pal.bar, true,
                           s_game_bar_scroll);
    }
    flush_rows_safe(desc, 21, 42);
}

void dmg_draw_status(const dmg_emulator_desc_t *desc, const char *msg, uint16_t color) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    if (msg && msg[0] != '\0') {
        s_status_active = true;
        dmg_clear_side_borders(desc);
        fill_rect_safe(desc, wx, 21, ww, 21, pal.bar);
        fill_rect_safe(desc, wx, 42, ww, 1, pal.border);
        draw_text_windowed(desc, msg, 27, color, pal.bar, true, 0u);
        flush_rows_safe(desc, 21, 42);
    } else if (s_status_active) {
        s_status_active = false;
        dmg_draw_game_bar(desc, s_active_game_name);
    }
}

/* =========================================================================
 * Clean Light Loading / Progress Screen
 * ========================================================================= */
void dmg_draw_loading_screen(const dmg_emulator_desc_t *desc, const char *title, const char *game_name, uint32_t total_size) {
    /* The resume intro owns the panel from core 1 while the game loads. */
    if (dmg_boot_intro_active()) return;
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    fill_screen_safe(desc, pal.bg);
    dmg_draw_top_bar(desc, desc->app_title, title);
    // Boards that load into PSRAM override this; everyone else keeps writing
    // flash and the default text stands.
    dmg_draw_bottom_bar(desc, desc->loading_status ? desc->loading_status
                                                  : "Writing flash cache...", "");

    /* Clean Light Card */
    int card_w = (ww == 266u) ? 230 : 272;
    int card_x = wx + (ww - card_w) / 2;
    fill_rect_safe(desc, card_x, 40, card_w, 160, pal.bar);
    fill_rect_safe(desc, card_x, 40, card_w, 1, pal.border);
    fill_rect_safe(desc, card_x, 199, card_w, 1, pal.border);
    fill_rect_safe(desc, card_x, 40, 1, 160, pal.border);
    fill_rect_safe(desc, card_x + card_w - 1, 40, 1, 160, pal.border);

    /* Title */
    text_safe(desc, title, card_x + 12, 54, pal.text_bar, pal.bar);

    /* Game name, wrapped.
     *
     * The frame here is the CARD's inner width, not the window's, because the
     * card border is what the glyphs must stay clear of. Using the window frame
     * here was the original bug: the window frame is 240 px in GB_MODE=1 while
     * the card only offers 230-24 = 206 px, so a 31-character name (248 px) ran
     * 42 px past the card edge and across its border.
     *
     * Rounded down to the glyph grid so the two margins come out even, same rule
     * as dmg_text_frame_w() but relative to the card. */
    const uint16_t name_frame_w = (uint16_t)(((uint16_t)(card_w - 24u) / DMG_GLYPH_PX) * DMG_GLYPH_PX);
    dmg_draw_wrapped_text(desc, game_name,
                          (int16_t)(card_x + 12), 74,
                          pal.text, pal.bar,
                          name_frame_w, 12u, 2u);

    /* Size Info */
    char size_buf[32];
    snprintf(size_buf, sizeof(size_buf), "Tamanho: %lu KB", (unsigned long)((total_size + 1023) / 1024));
    text_safe(desc, size_buf, card_x + 12, 104, pal.text, pal.bar);

    /* Progress bar track */
    int bar_w = (ww == 266u) ? 200 : 240;
    int bar_x = card_x + (card_w - bar_w) / 2;
    fill_rect_safe(desc, bar_x, 135, bar_w, 16, pal.bg);
    fill_rect_safe(desc, bar_x, 135, bar_w, 1, pal.border);
    fill_rect_safe(desc, bar_x, 150, bar_w, 1, pal.border);
    fill_rect_safe(desc, bar_x, 135, 1, 16, pal.border);
    fill_rect_safe(desc, bar_x + bar_w - 1, 135, 1, 16, pal.border);

    flush_rows_safe(desc, 0, 239);
    flush_safe(desc, true);
}

void dmg_update_loading_progress(const dmg_emulator_desc_t *desc, uint32_t bytes_loaded, uint32_t total_size) {
    watchdog_update();
    if (dmg_boot_intro_active()) return;
    if (total_size == 0) return;
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();
    int card_w = (ww == 266u) ? 230 : 272;
    int card_x = wx + (ww - card_w) / 2;
    int bar_w = (ww == 266u) ? 200 : 240;
    int bar_x = card_x + (card_w - bar_w) / 2;
    int track_inner_w = bar_w - 4;

    uint32_t fill_w = (uint32_t)((uint64_t)bytes_loaded * (uint32_t)track_inner_w / total_size);
    if (fill_w > (uint32_t)track_inner_w) fill_w = (uint32_t)track_inner_w;
    if (fill_w > 0) {
        fill_rect_safe(desc, bar_x + 2, 137, (int16_t)fill_w, 12, pal.sel);
    }
    char pct[16];
    snprintf(pct, sizeof(pct), "%3u%%", (unsigned)((uint64_t)bytes_loaded * 100u / total_size));
    int pct_x = bar_x + (bar_w - 32) / 2;
    text_safe(desc, pct, pct_x, 155, pal.text, pal.bar);
    flush_rows_safe(desc, 135, 170);
}

/* =========================================================================
 * In-Game Pause Menu
 * ========================================================================= */

/* Bit (slot - 1) is set when that slot holds a state for the game in play.
 *
 * Read once, when the menu opens. Nothing inside one visit can add or remove a
 * state -- saving ends the visit and returns to the game -- so a rescan per row
 * repaint would be four file opens per cursor move to learn the same thing. */
static uint8_t s_state_avail_mask = 0;

/* Both are defined further down; the redraw helper below needs them. */
static const char *get_ingame_hint(uint8_t cursor);
static void draw_ingame_row(const dmg_emulator_desc_t *desc, uint8_t idx, bool selected);

/* Blend fg into bg, keeping fg_pct of it. For the Load State slots that have
 * nothing in them: they have to read as unavailable without disappearing, and a
 * second grey would have to be defined for both palettes (RGB565 and RGB555)
 * and could drift between them. Blending the two that already exist cannot. */
static uint16_t dim_color(const dmg_emulator_desc_t *desc, uint16_t fg, uint16_t bg, uint8_t fg_pct) {
    const uint32_t f = fg;
    const uint32_t b = bg;
    const uint32_t rest = 100u - fg_pct;

    if (desc != NULL && desc->is_rgb555) {
        const uint32_t r = (((f >> 10) & 0x1Fu) * fg_pct + ((b >> 10) & 0x1Fu) * rest) / 100u;
        const uint32_t g = (((f >>  5) & 0x3Fu) * fg_pct + ((b >>  5) & 0x3Fu) * rest) / 100u;
        const uint32_t bl = ((f & 0x1Fu) * fg_pct + (b & 0x1Fu) * rest) / 100u;
        return (uint16_t)((r << 10) | (g << 5) | bl);
    }

    const uint32_t r = (((f >> 11) & 0x1Fu) * fg_pct + ((b >> 11) & 0x1Fu) * rest) / 100u;
    const uint32_t g = (((f >>  5) & 0x3Fu) * fg_pct + ((b >>  5) & 0x3Fu) * rest) / 100u;
    const uint32_t bl = ((f & 0x1Fu) * fg_pct + (b & 0x1Fu) * rest) / 100u;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* The Load State row, drawn one slot cell at a time.
 *
 * The four slots are a fixed run of characters inside the string this row used
 * to be built as ("Load State:   1 2 3 4"), so each cell sits at a known glyph
 * column and can carry its own colour. That is the whole reason this is not a
 * snprintf: one string has one colour, and the point of the row is that the
 * empty slots look empty. */
static void draw_load_state_slots(const dmg_emulator_desc_t *desc, uint16_t y, uint16_t bg) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    const int16_t frame_x0 = (int16_t)(dmg_get_window_x() + DMG_UI_TEXT_MARGIN);
    const uint16_t fg = (bg == pal.sel) ? pal.text_sel : pal.text;
    const uint16_t dim = dim_color(desc, fg, bg, 45u);

    draw_text_windowed(desc, "Load State:", y, fg, bg, false, 0u);

    for (uint8_t slot = 1; slot <= 4u; slot++) {
        const bool has = (s_state_avail_mask & (uint8_t)(1u << (slot - 1u))) != 0u;
        const bool sel = (g_dmg_config.load_slot == slot);

        /* Brackets only ever appear on a slot that can be loaded, because the
         * cursor cannot land on one that cannot. */
        char cell[4];
        cell[0] = (has && sel) ? '[' : ' ';
        cell[1] = (char)('0' + slot);
        cell[2] = (has && sel) ? ']' : ' ';
        cell[3] = '\0';

        /* "Load State:" is 11 glyphs, then three spaces, so slot N starts at
         * column 14 + 3 * (N - 1). Matches the spacing the row always had. */
        const int16_t cx = (int16_t)(frame_x0 +
                          (int16_t)((14u + 3u * ((uint16_t)slot - 1u)) * DMG_GLYPH_PX));
        text_safe(desc, cell, (uint16_t)cx, y, has ? fg : dim, bg);
    }
}

/* Put the Load State slot cursor on something loadable.
 *
 * Called when the row is entered. A slot left over from a previous game, or one
 * that was picked before it had anything in it, would otherwise park the cursor
 * where the player cannot use it and where the row is drawn dim. A slot that is
 * still valid is kept: parking on 3 on purpose should not move the cursor every
 * time the row is revisited. */
static void focus_load_slot(void) {
    if (s_state_avail_mask == 0u) return;
    if (g_dmg_config.load_slot < 1u || g_dmg_config.load_slot > 4u) {
        g_dmg_config.load_slot = 1u;
    }
    if (s_state_avail_mask & (uint8_t)(1u << (g_dmg_config.load_slot - 1u))) return;

    for (uint8_t slot = 1; slot <= 4u; slot++) {
        if (s_state_avail_mask & (uint8_t)(1u << (slot - 1u))) {
            g_dmg_config.load_slot = slot;
            return;
        }
    }
}

/* Move the Load State slot cursor to the next/previous slot that has a state,
 * wrapping inside the set that do. From slot 1 with states in 1 and 3, right
 * goes to 3 and then back to 1, never touching 2; with only slot 1 filled the
 * cursor stays where it is instead of stepping onto empty slots. */
static void step_load_slot(bool forward) {
    if (s_state_avail_mask == 0u) return;
    if (g_dmg_config.load_slot < 1u || g_dmg_config.load_slot > 4u) {
        g_dmg_config.load_slot = 1u;
    }

    uint8_t slot = (uint8_t)g_dmg_config.load_slot;
    for (uint8_t step = 0; step < 4u; step++) {
        slot = forward ? (uint8_t)((slot % 4u) + 1u)
                       : (uint8_t)((slot == 1u) ? 4u : (slot - 1u));
        if (s_state_avail_mask & (uint8_t)(1u << (slot - 1u))) {
            g_dmg_config.load_slot = slot;
            return;
        }
    }
}

static void scan_state_slots(const dmg_emulator_desc_t *desc) {
    s_state_avail_mask = 0;
    for (uint8_t slot = 1; slot <= 4u; slot++) {
        if (dmg_state_slot_exists(desc, s_active_game_name, slot)) {
            s_state_avail_mask |= (uint8_t)(1u << (slot - 1u));
        }
    }
    printf("[shared_menu] %s loadable state slots for '%s': 0x%02X\n",
           desc->app_title ? desc->app_title : "App", s_active_game_name,
           (unsigned)s_state_avail_mask);
}

/* Repaint the whole pause menu: bars, game name and all eight rows.
 *
 * The confirmation card and the custom submenu both cover the list, so both come
 * back through here before the loop carries on. The button state is not reset
 * here -- the caller waits for the release, because the press that opened the
 * overlay is still held when it returns. */
static void redraw_ingame_screen(const dmg_emulator_desc_t *desc, const dmg_palette_t *pal,
                                 uint8_t cursor) {
    s_status_active = false;
    fill_screen_safe(desc, pal->bg);
    draw_top_bar_impl(desc, desc->app_title, "Pause Menu", true);
    game_bar_scroll_reset();
    dmg_draw_game_bar(desc, s_active_game_name);
    dmg_draw_bottom_bar(desc, get_ingame_hint(cursor), "B: Back");
    for (uint8_t i = 0; i < DMG_UI_INGAME_ITEM_COUNT; i++) {
        draw_ingame_row(desc, i, i == cursor);
    }
    flush_safe(desc, true);
}

/* Modal Yes/Cancel card, drawn over the menu. Blocks until A or B and reports
 * which one.
 *
 * Two answers on one line rather than a cursor between them: there is nothing to
 * choose between beyond the two keys named on screen, so a selection state would
 * only add a step between the player and the answer.
 *
 * Waits for the release BEFORE reading, not after drawing. The A press that
 * opened the card is still down when this runs, so reading first would confirm
 * the dialog with the same press that summoned it. */
static bool confirm_dialog(const dmg_emulator_desc_t *desc, const dmg_palette_t *pal,
                           const char *title, const char *detail) {
    const uint16_t ww = dmg_get_window_w();
    const int card_w = (ww == DMG_GB_WINDOW_W) ? 230 : 272;
    const int card_h = 86;
    const int card_x = (int)(dmg_get_window_x() + (ww - (uint16_t)card_w) / 2u);
    const int card_y = (240 - card_h) / 2;

    dmg_clear_side_borders(desc);
    fill_rect_safe(desc, card_x, card_y, card_w, card_h, pal->bar);
    fill_rect_safe(desc, card_x, card_y, card_w, 1, pal->border);
    fill_rect_safe(desc, card_x, card_y + card_h - 1, card_w, 1, pal->border);
    fill_rect_safe(desc, card_x, card_y, 1, card_h, pal->border);
    fill_rect_safe(desc, card_x + card_w - 1, card_y, 1, card_h, pal->border);

    if (title != NULL) {
        draw_text_windowed(desc, title, (uint16_t)(card_y + 14), pal->text_bar, pal->bar, true, 0u);
    }
    if (detail != NULL && detail[0] != '\0') {
        /* The card's own frame, not the window's: the card border is what the
         * text has to stay clear of. */
        const uint16_t detail_frame_w =
            (uint16_t)(((uint16_t)(card_w - 24) / DMG_GLYPH_PX) * DMG_GLYPH_PX);
        dmg_draw_wrapped_text(desc, detail, (int16_t)(card_x + 12),
                              (int16_t)(card_y + 32), pal->text, pal->bar,
                              detail_frame_w, 12u, 2u);
    }
    draw_text_windowed(desc, "(A) Yes   (B) Cancel",
                       (uint16_t)(card_y + card_h - 24), pal->text, pal->bar, true, 0u);
    flush_safe(desc, true);

    dmg_wait_for_all_buttons_released();

    while (true) {
        watchdog_update();
        bool up, down, left, right, a, b, select, start;
        dmg_read_ui_buttons(&up, &down, &left, &right, &a, &b, &select, &start);
        (void)up; (void)down; (void)left; (void)right; (void)select; (void)start;

        if (a) return true;
        if (b) return false;

        tight_loop_contents();
        sleep_ms(20);
    }
}

/* Low-battery cutoff, from a menu: a short red warning card, then the same
 * standby as the power switch. Wakes on a charger or a switch cycle; the caller
 * repaints the screen afterwards. */
static void battery_shutdown(const dmg_emulator_desc_t *desc) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);
    printf("[battery] %u mV: low-battery standby\n", (unsigned)s_bs.mv);

    const uint16_t ww = dmg_get_window_w();
    const int card_w = (ww == DMG_GB_WINDOW_W) ? 230 : 272;
    const int card_h = 70;
    const int card_x = (int)(dmg_get_window_x() + (ww - (uint16_t)card_w) / 2u);
    const int card_y = (240 - card_h) / 2;

    dmg_clear_side_borders(desc);
    fill_rect_safe(desc, card_x, card_y, card_w, card_h, pal.bar);
    fill_rect_safe(desc, card_x, card_y, card_w, 2, pal.sel);
    fill_rect_safe(desc, card_x, card_y + card_h - 2, card_w, 2, pal.sel);
    fill_rect_safe(desc, card_x, card_y, 2, card_h, pal.sel);
    fill_rect_safe(desc, card_x + card_w - 2, card_y, 2, card_h, pal.sel);
    draw_text_windowed(desc, "Battery low", (uint16_t)(card_y + 18), pal.sel, pal.bar, true, 0u);
    draw_text_windowed(desc, "Charge the battery", (uint16_t)(card_y + 42), pal.text, pal.bar, true, 0u);
    flush_safe(desc, true);

    for (int i = 0; i < 40; i++) {   /* ~2 s */
        watchdog_update();
        sleep_ms(50);
    }

    /* Standby by save state: in the pause menu the game is saved first; in the
     * selector it just sleeps. Wakes on a charger or a switch cycle and
     * reboots into the same place. Never returns. */
    if (dmg_power_resume_enabled()) {
        dmg_power_off_with_resume(desc, DMG_POWER_REASON_BATTERY);
    }

    dmg_power_enter_battery_standby(desc, battery_charger_cb);

    /* Woke up. Without a charger the cell must recover to the release level
     * before the console runs again (see battery_cutoff_due). */
    s_cutoff_pending = false;
    s_low_count = 0;
    battery_sample();
    s_relock = !s_bs.usb;
    s_bs_next_ms = to_ms_since_boot(get_absolute_time()) + DMG_BATT_PERIOD_MS;
    s_shown_valid = false;
    dmg_wait_for_all_buttons_released();
}

static const char *get_ingame_hint(uint8_t cursor) {
    switch (cursor) {
        case 0:
            return "A: Save  L/R: Slot";
        case 1:
            return (s_state_avail_mask != 0u) ? "A: Load  L/R: Slot" : "No saved states";
        case 2:
            return "A / L/R: Toggle";
        case 3:
            return "A: Open Submenu";
        case 4:
        case 5:
            return "L/R: Adjust  A: Cycle";
        case 6:
            return "A: Reset Game";
        case 7:
            return "A: Save & Exit";
        default:
            return "";
    }
}

static void draw_ingame_row(const dmg_emulator_desc_t *desc, uint8_t idx, bool selected) {
    if (idx >= DMG_UI_INGAME_ITEM_COUNT) return;
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? pal.sel : pal.bg;

    fill_rect_safe(desc, wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    /* Drawn outside the string path below: its slots need different colours. */
    if (idx == 1) {
        draw_load_state_slots(desc, (uint16_t)(y + 7u), bg);
        flush_rows_safe(desc, y, y + DMG_UI_ITEM_HEIGHT - 1);
        return;
    }

    char buf[48];
    buf[0] = '\0';

    if (idx == 0) {
        char s1 = (g_dmg_config.save_slot == 1) ? '[' : ' ';
        char e1 = (g_dmg_config.save_slot == 1) ? ']' : ' ';
        char s2 = (g_dmg_config.save_slot == 2) ? '[' : ' ';
        char e2 = (g_dmg_config.save_slot == 2) ? ']' : ' ';
        char s3 = (g_dmg_config.save_slot == 3) ? '[' : ' ';
        char e3 = (g_dmg_config.save_slot == 3) ? ']' : ' ';
        char s4 = (g_dmg_config.save_slot == 4) ? '[' : ' ';
        char e4 = (g_dmg_config.save_slot == 4) ? ']' : ' ';
        snprintf(buf, sizeof(buf), "Save State:   %c1%c %c2%c %c3%c %c4%c", s1, e1, s2, e2, s3, e3, s4, e4);
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Speed:        %s", g_dmg_config.turbo_mode ? "< 2x >" : "< 1x >");
    } else if (idx == 3) {
        const char *sub_name = desc->custom_submenu_title ? desc->custom_submenu_title : "Image >";
        snprintf(buf, sizeof(buf), "%s", sub_name);
    } else if (idx == 4) {
        if (g_dmg_config.volume_level == 0) {
            snprintf(buf, sizeof(buf), "Volume:       < Mute >");
        } else {
            snprintf(buf, sizeof(buf), "Volume:       < %u >", (unsigned)g_dmg_config.volume_level);
        }
    } else if (idx == 5) {
        snprintf(buf, sizeof(buf), "Brightness:   < %u >", (unsigned)g_dmg_config.brightness_level);
    } else if (idx == 6) {
        snprintf(buf, sizeof(buf), "Reset");
    } else if (idx == 7) {
        snprintf(buf, sizeof(buf), "Exit");
    }

    draw_text_windowed(desc, buf, (uint16_t)(y + 7u),
                       selected ? pal.text_sel : pal.text, bg, false, 0u);
    flush_rows_safe(desc, y, y + DMG_UI_ITEM_HEIGHT - 1);
}

/* Persists the configuration through the emulator's own entry point.
 *
 * dmg_config_save() writes g_dmg_config, but an emulator keeps its settings in
 * its own variables and only mirrors them into g_dmg_config inside its save
 * function. Going straight to dmg_config_save() here would therefore write a
 * half-stale record: the rows this menu edits (Speed, Volume, Brightness) are
 * already in g_dmg_config, but anything the custom "Image >" submenu changed in
 * the same visit is not, so touching the volume afterwards would overwrite the
 * new Scale on disk with the old one.
 *
 * desc->save_config is what makes the whole record current; the fallback keeps
 * an emulator that never provided one working as before. */
void dmg_config_commit(const dmg_emulator_desc_t *desc) {
    if (desc == NULL) return;
    dmg_flush_global_volume();
    if (desc->save_config != NULL) {
        desc->save_config();
    } else {
        dmg_config_save(desc);
    }
}

static dmg_action_t ingame_menu_impl(const dmg_emulator_desc_t *desc, const char *game_name);

/* The pause menu runs at a frame boundary with the blitter parked, which is
 * exactly where a save state is consistent: the power switch flipped OFF in
 * here saves the game before the standby (dmg_power_poll). */
dmg_action_t dmg_ingame_menu(const dmg_emulator_desc_t *desc, const char *game_name) {
    dmg_power_note_pause_menu(true);
    const dmg_action_t act = ingame_menu_impl(desc, game_name);
    dmg_power_note_pause_menu(false);
    return act;
}

static dmg_action_t ingame_menu_impl(const dmg_emulator_desc_t *desc, const char *game_name) {
    if (desc == NULL) return DMG_ACTION_RESUME;

    dmg_init_boot_mode();

    if (desc->display.stop_blitter) {
        desc->display.stop_blitter();
    }

    /* Save on the way in. The player may have changed something outside this
     * menu (a combo, the display submenu of a previous session) and the file is
     * what the next boot reads, so it must not be left behind by a visit that
     * changes nothing. Cheap: one small file, once per menu open. */
    dmg_config_commit(desc);

    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    strncpy(s_active_game_name, game_name ? game_name : "", sizeof(s_active_game_name) - 1);
    s_active_game_name[sizeof(s_active_game_name) - 1] = '\0';
    s_status_active = false;

    /* Which slots can actually be loaded, for this game. Read here, once,
     * because the row that shows them is redrawn from the mask on every cursor
     * move and a per-move probe would be four file opens per step. */
    scan_state_slots(desc);
    if (g_dmg_config.load_slot < 1u || g_dmg_config.load_slot > 4u) {
        g_dmg_config.load_slot = 1u;
    }
    focus_load_slot();

    uint8_t cursor = 0;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;

    dmg_wait_for_all_buttons_released();

    redraw_ingame_screen(desc, &pal, cursor);

    while (true) {
        watchdog_update();
        if (battery_tick(desc)) {
            redraw_ingame_screen(desc, &pal, cursor);
        }
        bool btn_up, btn_down, btn_left, btn_right, btn_a, btn_b, btn_select, btn_start;
        dmg_read_ui_buttons(&btn_up, &btn_down, &btn_left, &btn_right, &btn_a, &btn_b, &btn_select, &btn_start);

        bool pushed_up    = btn_up && !prev_up;
        bool pushed_down  = btn_down && !prev_down;
        bool pushed_left  = btn_left && !prev_left;
        bool pushed_right = btn_right && !prev_right;
        bool pushed_a     = btn_a && !prev_a;
        bool pushed_b     = btn_b && !prev_b;

        prev_up    = btn_up;
        prev_down  = btn_down;
        prev_left  = btn_left;
        prev_right = btn_right;
        prev_a     = btn_a;
        prev_b     = btn_b;

        /* A game name too long for the window slides under the margins so the
         * rest of it can be read, the same treatment the ROM list gets. Only the
         * name bar is touched, and only while no status message is showing --
         * the status replaces the bar, so stepping both would fight. */
        if (!s_status_active) {
            char probe[44];
            const uint16_t overflow = rom_name_px(s_active_game_name, probe,
                                                  sizeof(probe),
                                                  dmg_get_window_w());

            if (overflow > 0) {
                s_game_bar_ms += DMG_ROM_SCROLL_STEP_MS;

                /* At an end, hold still for a beat, then reverse. Reversing
                 * rather than wrapping is what makes the name readable: a wrap
                 * throws away the reader's place mid-word. */
                const bool at_end =
                    (s_game_bar_dir > 0) ? (s_game_bar_scroll >= overflow)
                                         : (s_game_bar_scroll == 0);

                if (at_end) {
                    if (s_game_bar_ms >= DMG_ROM_SCROLL_EDGE_MS) {
                        s_game_bar_ms = 0;
                        s_game_bar_dir = (int8_t)-s_game_bar_dir;
                    }
                } else if (s_game_bar_ms >= DMG_ROM_SCROLL_STEP_MS) {
                    s_game_bar_ms = 0;
                    if (s_game_bar_dir > 0) {
                        s_game_bar_scroll =
                            (uint16_t)(s_game_bar_scroll + DMG_ROM_SCROLL_STEP_PX);
                        if (s_game_bar_scroll > overflow)
                            s_game_bar_scroll = overflow;
                    } else {
                        /* Saturating subtraction: at scroll_px < step this would
                         * wrap to ~65000 and throw the text off the panel. */
                        s_game_bar_scroll =
                            (s_game_bar_scroll > DMG_ROM_SCROLL_STEP_PX)
                                ? (uint16_t)(s_game_bar_scroll - DMG_ROM_SCROLL_STEP_PX)
                                : 0u;
                    }
                    dmg_draw_game_bar(desc, s_active_game_name);
                }
            } else if (s_game_bar_scroll != 0) {
                game_bar_scroll_reset();
                dmg_draw_game_bar(desc, s_active_game_name);
            }
        }

        if (pushed_up) {
            uint8_t prev = cursor;
            cursor = (cursor == 0) ? (DMG_UI_INGAME_ITEM_COUNT - 1) : (cursor - 1);
            if (cursor == 1) focus_load_slot();
            draw_ingame_row(desc, prev, false);
            draw_ingame_row(desc, cursor, true);
            dmg_draw_bottom_bar(desc, get_ingame_hint(cursor), "B: Back");
            dmg_draw_status(desc, "", 0);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == (DMG_UI_INGAME_ITEM_COUNT - 1)) ? 0 : (cursor + 1);
            if (cursor == 1) focus_load_slot();
            draw_ingame_row(desc, prev, false);
            draw_ingame_row(desc, cursor, true);
            dmg_draw_bottom_bar(desc, get_ingame_hint(cursor), "B: Back");
            dmg_draw_status(desc, "", 0);
        }

        if (pushed_left || pushed_right) {
            if (cursor == 0) {
                if (pushed_right) {
                    g_dmg_config.save_slot = (g_dmg_config.save_slot % 4) + 1;
                } else {
                    g_dmg_config.save_slot = (g_dmg_config.save_slot == 1) ? 4 : (g_dmg_config.save_slot - 1);
                }
                draw_ingame_row(desc, 0, true);
                dmg_draw_status(desc, "", 0);
            } else if (cursor == 1) {
                /* Skips the slots with nothing in them: from 1 with states in 1
                 * and 3, right lands on 3. */
                step_load_slot(pushed_right);
                draw_ingame_row(desc, 1, true);
                dmg_draw_status(desc, "", 0);
            } else if (cursor == 2) {
                g_dmg_config.turbo_mode = !g_dmg_config.turbo_mode;
                if (desc->apply_speed) desc->apply_speed(g_dmg_config.turbo_mode != 0);
                dmg_config_commit(desc);
                draw_ingame_row(desc, 2, true);
                dmg_draw_status(desc, "", 0);
            } else if (cursor == 4) {
                if (pushed_right) {
                    if (g_dmg_config.volume_level < 5) g_dmg_config.volume_level++;
                } else {
                    if (g_dmg_config.volume_level > 0) g_dmg_config.volume_level--;
                }
                dmg_set_global_volume((uint8_t)g_dmg_config.volume_level, desc);
                draw_ingame_row(desc, 4, true);
                dmg_draw_status(desc, "", 0);
            } else if (cursor == 5) {
                if (pushed_right) {
                    if (g_dmg_config.brightness_level < 5) g_dmg_config.brightness_level++;
                } else {
                    if (g_dmg_config.brightness_level > 1) g_dmg_config.brightness_level--;
                }
                if (desc->apply_brightness) desc->apply_brightness((uint8_t)g_dmg_config.brightness_level);
                dmg_config_commit(desc);
                draw_ingame_row(desc, 5, true);
                dmg_draw_status(desc, "", 0);
            }
        }

        if (pushed_a) {
            if (cursor == 0) {
                /* Save State */
                if (desc->save_state) {
                    const uint8_t slot = (uint8_t)g_dmg_config.save_slot;
                    bool go = true;

                    /* Overwriting is silent loss: the state it replaces is the
                     * only copy of that point in the game, and the row gives no
                     * sign that the slot is occupied. Ask first. */
                    if (s_state_avail_mask & (uint8_t)(1u << (slot - 1u))) {
                        go = confirm_dialog(desc, &pal, "Savedata exists,", "overwrite?");
                        dmg_wait_for_all_buttons_released();
                        prev_up = prev_down = prev_left = prev_right = true;
                        prev_a = prev_b = true;
                    }

                    if (!go) {
                        redraw_ingame_screen(desc, &pal, cursor);
                    } else {
                        bool ok = desc->save_state(s_active_game_name, slot);
                        if (ok) {
                            dmg_draw_status(desc, "State Salvo!", pal.status_ok);
                            dmg_config_commit(desc);
                            sleep_ms(600);
                            dmg_wait_for_all_buttons_released();
                            return DMG_ACTION_RESUME;
                        } else {
                            dmg_draw_status(desc, "Erro ao Salvar State", pal.status_err);
                        }
                    }
                }
            } else if (cursor == 1) {
                /* Load State */
                if (desc->load_state) {
                    if (s_state_avail_mask == 0u) {
                        /* Nothing on the card for this game, so there is no slot
                         * to even offer. */
                        dmg_draw_status(desc, "No Saved States", pal.status_err);
                    } else {
                        const uint8_t slot = (uint8_t)g_dmg_config.load_slot;
                        const bool go = confirm_dialog(desc, &pal, "Load State?", NULL);
                        dmg_wait_for_all_buttons_released();
                        prev_up = prev_down = prev_left = prev_right = true;
                        prev_a = prev_b = true;

                        if (go) {
                            bool ok = desc->load_state(s_active_game_name, slot);
                            if (ok) {
                                dmg_draw_status(desc, "State Carregado!", pal.status_ok);
                                dmg_config_commit(desc);
                                sleep_ms(600);
                                dmg_wait_for_all_buttons_released();
                                return DMG_ACTION_RESUME;
                            } else {
                                dmg_draw_status(desc, "Empty Load State", pal.status_err);
                            }
                        } else {
                            redraw_ingame_screen(desc, &pal, cursor);
                        }
                    }
                }
            } else if (cursor == 2) {
                /* Turbo Mode Toggle */
                g_dmg_config.turbo_mode = !g_dmg_config.turbo_mode;
                if (desc->apply_speed) desc->apply_speed(g_dmg_config.turbo_mode != 0);
                dmg_config_commit(desc);
                draw_ingame_row(desc, 2, true);
                dmg_draw_status(desc, "", 0);
            } else if (cursor == 3) {
                /* Open Submenu */
                if (desc->custom_submenu) {
                    desc->custom_submenu(desc, s_active_game_name);
                    /* Redraw in-game menu upon return */
                    redraw_ingame_screen(desc, &pal, cursor);
                    dmg_wait_for_all_buttons_released();
                    prev_up = true; prev_down = true; prev_left = true; prev_right = true;
                    prev_a = true; prev_b = true;
                }
            } else if (cursor == 4) {
                /* Cycle Volume 0..5 (0 -> 1 -> ... -> 5 -> 0) */
                g_dmg_config.volume_level = (g_dmg_config.volume_level + 1) % 6;
                dmg_set_global_volume((uint8_t)g_dmg_config.volume_level, desc);
                draw_ingame_row(desc, 4, true);
                dmg_draw_status(desc, "", 0);
            } else if (cursor == 5) {
                /* Cycle Brightness */
                g_dmg_config.brightness_level = (g_dmg_config.brightness_level % 5) + 1;
                if (desc->apply_brightness) desc->apply_brightness((uint8_t)g_dmg_config.brightness_level);
                dmg_config_commit(desc);
                draw_ingame_row(desc, 5, true);
                dmg_draw_status(desc, "", 0);
            } else if (cursor == 6) {
                /* Reset */
                dmg_draw_status(desc, "Resetting game...", pal.text_bar);
                sleep_ms(400);
                dmg_wait_for_all_buttons_released();
                dmg_config_commit(desc);
                return DMG_ACTION_RESET;
            } else if (cursor == 7) {
                /* Exit */
                if (desc->exit_game) {
                    desc->exit_game();
                }
                dmg_wait_for_all_buttons_released();
                dmg_config_commit(desc);
                return DMG_ACTION_EXIT;
            }
        }

        if (pushed_b) {
            dmg_wait_for_all_buttons_released();
            dmg_config_commit(desc);
            return DMG_ACTION_RESUME;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}

/* =========================================================================
 * Clean Light ROM File Selector
 * ========================================================================= */
static char s_current_rom_dir[64] = "";
static uint16_t s_rom_selector_page = 0;
static uint8_t  s_rom_selector_index = 0;

const char *dmg_get_current_rom_dir(void) {
    return s_current_rom_dir;
}

void dmg_set_current_rom_dir(const char *dir) {
    if (dir) {
        strncpy(s_current_rom_dir, dir, sizeof(s_current_rom_dir) - 1);
        s_current_rom_dir[sizeof(s_current_rom_dir) - 1] = '\0';
    }
}

static bool find_active_rom_dir(const dmg_emulator_desc_t *desc, char *out_dir, size_t out_dir_size) {
    if (desc == NULL || out_dir == NULL) return false;
    DIR dj;
    FILINFO fno;

    /* Check configured candidate dirs */
    if (desc->candidate_dirs) {
        for (size_t i = 0; desc->candidate_dirs[i] != NULL; i++) {
            const char *cand = desc->candidate_dirs[i];
            if (cand[0] == '\0') continue;
            FRESULT fr = f_opendir(&dj, cand);
            if (fr == FR_OK) {
                bool found = false;
                while (f_readdir(&dj, &fno) == FR_OK && fno.fname[0] != '\0') {
                    if (fno.fname[0] == '.') continue;
                    if (fno.fattrib & AM_DIR) {
                        if (desc->is_rom_file != NULL && desc->is_rom_file(fno.fname)) {
                            found = true;
                            break;
                        }
                    } else {
                        if (desc->is_rom_file == NULL || desc->is_rom_file(fno.fname)) {
                            found = true;
                            break;
                        }
                    }
                }
                f_closedir(&dj);
                if (found) {
                    snprintf(out_dir, out_dir_size, "%s", cand);
                    printf("[shared_menu] Found active ROM directory: '%s'\n", out_dir);
                    return true;
                }
            }
        }
    }

    /* Fallback: default_rom_dir */
    if (desc->default_rom_dir) {
        snprintf(out_dir, out_dir_size, "%s", desc->default_rom_dir);
        return true;
    }
    return false;
}

static void draw_menu_top_bar(const dmg_emulator_desc_t *desc, uint16_t page_num) {
    char page_str[16];
    snprintf(page_str, sizeof(page_str), "Page %u", (unsigned int)(page_num + 1u));
    draw_top_bar_impl(desc, desc->app_title, page_str, true);
}

static void draw_menu_bottom_bar(const dmg_emulator_desc_t *desc) {
    const char *r_text = dmg_is_launched_from_bootloader() ? "[SEL+START] Bootloader" : "< >: Page";
    dmg_draw_bottom_bar(desc, "A: Start", r_text);
}


static void draw_rom_item(const dmg_emulator_desc_t *desc, const char *filename, uint8_t row_idx, bool selected, uint16_t scroll_px) {
    if (row_idx >= DMG_UI_ROM_SELECTOR_PAGE_SIZE) return;
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    uint16_t y = (uint16_t)(DMG_UI_ROM_LIST_START_Y + (uint16_t)row_idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? pal.sel : pal.bg;

    fill_rect_safe(desc, wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    if (filename && filename[0] != '\0') {
        char disp_name[44];
        (void)rom_name_px(filename, disp_name, sizeof(disp_name), ww);

        /* Left aligned against the margin; draw_text_windowed owns the clipping
         * and the scroll clamp, so a scrolled name can start left of the window
         * without wrapping to a huge unsigned coordinate and disappearing. */
        draw_text_windowed(desc, disp_name, (uint16_t)(y + 7u),
                           selected ? pal.text_sel : pal.text, bg, false, scroll_px);
    }

    flush_rows_safe(desc, y, y + DMG_UI_ITEM_HEIGHT - 1);
}

/* ---------------------------------------------------------------------------
 * ROM list paging
 *
 * The list is served in raw directory order -- whatever order the files sit in
 * on the card -- and each page is one walk of the directory that skips the
 * entries belonging to earlier pages.
 *
 * Three earlier designs are worth recording, because each of them was wrong in a
 * way that did not look like a bug:
 *
 *   - Storing every name in a 4096-byte pool plus an offset table, so the whole
 *     list could be sorted before slicing a page. 4096 bytes holds about 102
 *     names of ~40 characters, so a 279-game folder silently showed 102 of them
 *     across 12 pages. The only symptom was a game that was not in the list.
 *
 *   - Keeping every name and calling the OS qsort(). Solves the truncation but
 *     costs the same RAM as the pool it replaced, so the cap just moves.
 *
 *   - Resolving one row per directory walk, walking the chain of "smallest name
 *     greater than the previous one" to produce alphabetical order without
 *     storing anything. Correct, and it lists everything -- but it needs nine
 *     walks to fill a page, and a walk is a real cost: FatFs caches the current
 *     sector (FF_MAX_SS = 512), so a 279-entry directory is ~35 sector reads.
 *     Nine walks is ~315, and paging backwards past the cached boundary falls
 *     back to re-deriving the chain from the first name, which is ~9500 sector
 *     reads for the last page. Rapid paging then stacked those up and tripped
 *     the 6 s watchdog, which is what it looked like: the console hanging.
 *
 * Reading raw needs none of that. The cost is O(start + 9) entries with a hard
 * ceiling of one walk, because the loop stops the moment the page is full:
 *
 *     page 0   ~0.3 walks   ~1 sector
 *     page 15  ~1 walk      ~7 sectors
 *     page 30  ~1 walk      ~35 sectors
 *
 * The order is the order the ROMs were written to the card. That is also stable
 * under adding games: new files land at the end, so existing positions do not
 * shift. Sorting would reshuffle everything after each insertion.
 *
 * There is no pool and no entry cap, so the number of games is bounded only by
 * how many fit on the card.
 * ------------------------------------------------------------------------ */
static uint16_t rom_display_page(const dmg_emulator_desc_t *desc, char filename[DMG_UI_ROM_SELECTOR_PAGE_SIZE][256], uint16_t num_page) {
    for (uint8_t i = 0; i < DMG_UI_ROM_SELECTOR_PAGE_SIZE; i++) {
        filename[i][0] = '\0';
    }

    if (s_current_rom_dir[0] == '\0') {
        return 0;
    }

    const uint32_t start = (uint32_t)num_page * DMG_UI_ROM_SELECTOR_PAGE_SIZE;

    DIR dj;
    FILINFO fno;
    if (f_opendir(&dj, s_current_rom_dir) != FR_OK) {
        return 0;
    }

    uint32_t seen = 0;
    uint16_t num_file = 0;

    while (f_readdir(&dj, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fname[0] == '.') continue;
        if (fno.fattrib & AM_DIR) {
            if (desc->is_rom_file == NULL || !desc->is_rom_file(fno.fname)) continue;
        } else {
            if (desc->is_rom_file != NULL && !desc->is_rom_file(fno.fname)) continue;
        }

        /* Entries owned by earlier pages. Still counted, not skipped silently:
         * the page number is a position in the filtered list, so directories
         * and non-ROM files must not advance it. */
        if (seen < start) {
            seen++;
            continue;
        }

        watchdog_update();

        strncpy(filename[num_file], fno.fname, 255u);
        filename[num_file][255] = '\0';
        num_file++;

        if (num_file >= DMG_UI_ROM_SELECTOR_PAGE_SIZE) {
            break;   /* page is full: no reason to read the rest of the card */
        }
    }
    f_closedir(&dj);

    if (num_file > 0) {
        draw_menu_top_bar(desc, num_page);
        draw_menu_bottom_bar(desc);
        for (uint8_t ifile = 0; ifile < num_file; ifile++) {
            draw_rom_item(desc, filename[ifile], ifile, ifile == s_rom_selector_index, 0u);
        }
        uint16_t wx = dmg_get_window_x();
        uint16_t ww = dmg_get_window_w();
        for (uint8_t ifile = num_file; ifile < DMG_UI_ROM_SELECTOR_PAGE_SIZE; ifile++) {
            uint16_t y = (uint16_t)(DMG_UI_ROM_LIST_START_Y + (uint16_t)ifile * DMG_UI_ITEM_HEIGHT);
            dmg_palette_t pal;
            dmg_get_palette(desc, &pal);
            fill_rect_safe(desc, wx, y, ww, DMG_UI_ITEM_HEIGHT, pal.bg);
            flush_rows_safe(desc, y, y + DMG_UI_ITEM_HEIGHT - 1);
        }
        flush_safe(desc, true);
    }
    return num_file;
}

bool dmg_rom_selector(const dmg_emulator_desc_t *desc, char *out_selected_rom, size_t max_len) {
    if (desc == NULL || out_selected_rom == NULL || max_len == 0) return false;

    dmg_init_boot_mode();
    dmg_flush_global_volume();

    if (desc->display.stop_blitter) {
        desc->display.stop_blitter();
    }
    dmg_wait_for_all_buttons_released();

    if (s_current_rom_dir[0] == '\0') {
        if (!find_active_rom_dir(desc, s_current_rom_dir, sizeof(s_current_rom_dir))) {
            snprintf(s_current_rom_dir, sizeof(s_current_rom_dir), "%s", desc->default_rom_dir ? desc->default_rom_dir : "0:/roms");
        }
    }

    char filename[DMG_UI_ROM_SELECTOR_PAGE_SIZE][256];
    uint16_t num_file = rom_display_page(desc, filename, s_rom_selector_page);
    if (num_file == 0 && s_rom_selector_page > 0) {
        s_rom_selector_page = 0;
        num_file = rom_display_page(desc, filename, s_rom_selector_page);
    }

    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    if (num_file == 0) {
        fill_screen_safe(desc, pal.bg);
        draw_menu_top_bar(desc, 0);
        draw_menu_bottom_bar(desc);
        text_safe(desc, "No ROM files found!", 20, 70, pal.status_err, pal.bg);
        char dir_msg[64];
        snprintf(dir_msg, sizeof(dir_msg), "Folder: %s is empty", s_current_rom_dir);
        text_safe(desc, dir_msg, 20, 95, pal.text, pal.bg);
        text_safe(desc, "Press SELECT+START to exit", 20, 115, pal.text_bar, pal.bg);
        flush_safe(desc, true);

        dmg_wait_for_all_buttons_released();
        /* Arm power switch with 1-second delay after displaying the message */
        dmg_power_arm_menu_loaded(1000);
        while (true) {
            watchdog_update();
            bool up, down, left, right, a, b, select, start;
            dmg_read_ui_buttons(&up, &down, &left, &right, &a, &b, &select, &start);
            if (select && start) {
                if (dmg_is_launched_from_bootloader()) {
                    dmg_reboot_to_bootloader();
                }
            }
            sleep_ms(30);
        }
    }

    if (s_rom_selector_index >= num_file) {
        s_rom_selector_index = 0;
    }

    /* Marquee for the highlighted row. Starts at 0 and resets whenever the selection
     * moves, so the name reads from the beginning first. Once moving it reverses
     * at each end instead of wrapping. */
    uint16_t scroll_px = 0;
    uint64_t scroll_ms = 0;
    /* -1 sliding left (higher offset), +1 sliding right. */
    int8_t scroll_dir = 1;

    /* Initial highlight */
    draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);

    /* The emulator menu is now fully loaded, drawn, and displayed on screen!
     * Arm the power switch with a 1-second delay (1000 ms) before allowing
     * any power-off / standby action, ensuring flash and SD operations are
     * completely quiescent and avoiding corruption if the switch was toggled
     * during the bootloader flashing process. */
    dmg_power_arm_menu_loaded(1000);

    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true, prev_select = true, prev_start = true;

    while (true) {
        watchdog_update();
        if (battery_tick(desc)) {
            fill_screen_safe(desc, pal.bg);
            num_file = rom_display_page(desc, filename, s_rom_selector_page);
            if (num_file > 0 && s_rom_selector_index < num_file) {
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);
            }
        }
        bool btn_up, btn_down, btn_left, btn_right, btn_a, btn_b, btn_select, btn_start;
        dmg_read_ui_buttons(&btn_up, &btn_down, &btn_left, &btn_right, &btn_a, &btn_b, &btn_select, &btn_start);

        bool pushed_up    = btn_up && !prev_up;
        bool pushed_down  = btn_down && !prev_down;
        bool pushed_left  = btn_left && !prev_left;
        bool pushed_right = btn_right && !prev_right;
        bool pushed_a     = btn_a && !prev_a;
        bool pushed_b     = btn_b && !prev_b;
        bool combo_boot   = btn_select && btn_start;

        prev_up     = btn_up;
        prev_down   = btn_down;
        prev_left   = btn_left;
        prev_right  = btn_right;
        prev_a      = btn_a;
        prev_b      = btn_b;
        prev_select = btn_select;
        prev_start  = btn_start;

        if (combo_boot) {
            if (dmg_is_launched_from_bootloader()) {
                dmg_reboot_to_bootloader();
            }
        }

        if (pushed_up) {
            if (s_rom_selector_index > 0) {
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, false, 0u);
                s_rom_selector_index--;
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);
            }
        } else if (pushed_down) {
            if (s_rom_selector_index + 1 < num_file) {
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, false, 0u);
                s_rom_selector_index++;
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);
            }
        }

        if (pushed_left) {
            if (s_rom_selector_page > 0) {
                s_rom_selector_page--;
                s_rom_selector_index = 0;
                num_file = rom_display_page(desc, filename, s_rom_selector_page);
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);
            }
        } else if (pushed_right) {
            s_rom_selector_page++;
            uint16_t new_num = rom_display_page(desc, filename, s_rom_selector_page);
            if (new_num == 0) {
                s_rom_selector_page--;
                num_file = rom_display_page(desc, filename, s_rom_selector_page);
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);
            } else {
                num_file = new_num;
                s_rom_selector_index = 0;
                draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);
            }
        }

        /* Any selection change restarts the marquee from the left. Done here
         * rather than inside each branch so no navigation path can miss it. */
        if (pushed_up || pushed_down || pushed_left || pushed_right) {
            scroll_px = 0;
            scroll_ms = 0;
            scroll_dir = 1;
        }

        /* Advance the marquee. Only the highlighted row can overflow in a way
         * worth showing, so the other rows stay still. At each end the name holds
         * for a beat and then reverses -- going back the way it came rather than
         * snapping to the start, so the reader keeps their place. */
        if (num_file > 0) {
            char probe[44];
            const uint16_t overflow = rom_name_px(filename[s_rom_selector_index],
                                                  probe, sizeof(probe),
                                                  dmg_get_window_w());

            if (overflow > 0) {
                scroll_ms += DMG_ROM_SCROLL_STEP_MS;

                const bool at_end = (scroll_dir > 0) ? (scroll_px >= overflow)
                                                     : (scroll_px == 0);

                if (at_end) {
                    if (scroll_ms >= DMG_ROM_SCROLL_EDGE_MS) {
                        scroll_ms = 0;
                        scroll_dir = (int8_t)-scroll_dir;
                    }
                } else if (scroll_ms >= DMG_ROM_SCROLL_STEP_MS) {
                    scroll_ms = 0;

                    if (scroll_dir > 0) {
                        scroll_px = (uint16_t)(scroll_px + DMG_ROM_SCROLL_STEP_PX);
                        if (scroll_px > overflow) scroll_px = overflow;
                    } else {
                        /* Saturating: plain subtraction would wrap below zero to
                         * ~65000 and throw the text off the panel. */
                        scroll_px = (scroll_px > DMG_ROM_SCROLL_STEP_PX)
                                        ? (uint16_t)(scroll_px - DMG_ROM_SCROLL_STEP_PX)
                                        : 0u;
                    }

                    draw_rom_item(desc, filename[s_rom_selector_index],
                                  s_rom_selector_index, true, scroll_px);
                }
            } else if (scroll_px != 0) {
                /* Selection changed onto a short name: put it back at rest. */
                scroll_px = 0;
                scroll_dir = 1;
                draw_rom_item(desc, filename[s_rom_selector_index],
                              s_rom_selector_index, true, 0u);
            }
        }

        if (pushed_a && num_file > 0) {
            strncpy(out_selected_rom, filename[s_rom_selector_index], max_len - 1);
            out_selected_rom[max_len - 1] = '\0';
            if (desc->on_load_rom) {
                if (!desc->on_load_rom(desc, out_selected_rom)) {
                    fill_screen_safe(desc, pal.bg);
                    rom_display_page(desc, filename, s_rom_selector_page);
                    draw_rom_item(desc, filename[s_rom_selector_index], s_rom_selector_index, true, 0u);
                    continue;
                }
            }
            dmg_wait_for_all_buttons_released();
            return true;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}
