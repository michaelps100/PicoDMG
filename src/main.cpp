#pragma GCC optimize("Ofast")
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cstdlib>
#include <hardware/flash.h>
#include <hardware/vreg.h>
#include <hardware/clocks.h>
#include <hardware/pll.h>
#include <hardware/pwm.h>
#include <hardware/sync.h>
#include <hardware/watchdog.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>

#include "ili9341.h"
#include "audio.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#include "PicoPlusPsram.h"

#include "ff.h"

#include "lynx/handy.h"

extern char __flash_binary_end;
#define FLASH_TARGET_OFFSET (((((uintptr_t)&__flash_binary_end - XIP_BASE) / FLASH_SECTOR_SIZE) + 4) * FLASH_SECTOR_SIZE)
static const uintptr_t rom_flash_xip = XIP_BASE + FLASH_TARGET_OFFSET;

/* Where the loaded cart actually lives. On the Core2350B this is a PSRAM buffer
 * (PSRAM_CS_PIN=47, 8 MB); on the ILI9341 board it stays the Flash XIP window
 * above. s_rom_in_psram is the single source of truth -- the PSRAM window lives
 * at 0x11000000, which no address comparison tests for, so comparing pointers
 * is not portable here. */
static uint8_t *s_rom_ptr = (uint8_t *)rom_flash_xip;
static bool s_rom_in_psram = false;

#define AUDIO_SAMPLE_RATE HANDY_AUDIO_SAMPLE_FREQ
#define AUDIO_BUFFER_LENGTH HANDY_AUDIO_BUFFER_LENGTH

/* =========================================================================
 * Build Target Configuration (PicoLINX)
 * ========================================================================= */
#define APP_TITLE            "PicoLYNX"
#define CONFIG_MAGIC         0x4C4E5843u /* "LNXC" */
#define CONFIG_PATH          "0:/saves/lynx/picolynx.cfg"
#define CONFIG_PATH_FALLBACK "/saves/lynx/picolynx.cfg"
#define SAVES_DIR            "0:/saves/lynx"
#define SAVES_DIR_FALLBACK   "/saves/lynx"
#define ROMS_DIR             "0:/roms/lynx"
#define ROMS_DIR_FALLBACK    "/roms/lynx"

#define LYNK_ROM_CACHE_MAX   0x400000u /* 4 MB ROM cache max */

#define ROM_ITEM_HEIGHT        22u
#define ROM_LIST_START_Y       21u
#define INGAME_LIST_START_Y    43u
#define ROM_SELECTOR_PAGE_SIZE 9u
#define INGAME_ITEM_COUNT      8u

/* =========================================================================
 * Persistent Configuration
 * ========================================================================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t scale_mode;       /* 0 Fit, 1 Stretch, 2 Integer */
    uint32_t grid_effect;      /* 0 None, 1 Grid */
    uint32_t interleave;       /* 0 Progressive, 1 Interleave */
    uint32_t turbo_mode;       /* 0 1x, 1 2x */
    uint32_t volume_level;     /* 1..5, default 3 */
    uint32_t brightness_level; /* 1..5, default 5 */
    uint32_t save_slot;        /* 1..4, default 1 */
    uint32_t load_slot;        /* 1..4, default 1 */
} emulator_config_t;

static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_grid_effect = false;
static bool persistent_shimmerless_enabled = true;
static bool persistent_interleave = false;
static bool persistent_turbo_mode = false;
static uint8_t persistent_volume_level = 3;
static uint8_t persistent_brightness_level = 5;
static uint8_t persistent_save_slot = 1;
static uint8_t persistent_load_slot = 1;

static bool s_request_pause_menu = false;
static bool s_request_exit_game = false;
static bool reset_requested = false;

static bool s_fps_overlay_enabled = false;
static uint32_t s_fps_value = 60;

static char __uninitialized_ram(selectedRom[FF_MAX_LFN]);
static uint32_t __uninitialized_ram(rom_size);

static FATFS fs;
bool reboot = false;

#if PICO_RP2350
#define CPU_FREQ_KHZ 360000u
#include "hardware/structs/qmi.h"
static void __no_inline_not_in_flash_func(apply_safe_qmi_timing)(void) {
    const uint32_t max_flash_khz = 94500;
    uint32_t flash_div = (CPU_FREQ_KHZ + max_flash_khz - 1) / max_flash_khz;
    if (flash_div < 4)
        flash_div = 4;
    uint32_t rxdelay = (3 * CPU_FREQ_KHZ + 189000) / 378000;
    if (rxdelay > 7)
        rxdelay = 7;
    uint32_t ints = save_and_disable_interrupts();
    qmi_hw->m[0].timing = 0x60000000u | (14u << 11) | (rxdelay << 8) | flash_div;
    __asm volatile("nop\nnop\nnop\nnop");
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(safe_flash_range_erase)(uint32_t offset, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, count);
    apply_safe_qmi_timing();
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(safe_flash_range_program)(uint32_t offset, const uint8_t *data, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, count);
    apply_safe_qmi_timing();
    restore_interrupts(ints);
}
#else
static inline void apply_safe_qmi_timing(void) {}
static inline void safe_flash_range_erase(uint32_t offset, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, count);
    restore_interrupts(ints);
}
static inline void safe_flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, count);
    restore_interrupts(ints);
}
#endif
CSystem *lynx;
alignas(4) uint8_t SCREEN[HANDY_SCREEN_HEIGHT][HANDY_SCREEN_WIDTH];
alignas(4) int16_t audio_buffer[AUDIO_BUFFER_LENGTH];

i2s_config_t i2s_config;

static uint16_t lynx_palette_rgb565[256];

static void get_sanitized_basename(const char *game_name, char *out, size_t out_len);

/* Static buffer for ROM directory */
static char current_rom_dir[64] = ROMS_DIR;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;

#define ROM_UI_BG_COLOR       DMG_RGB565_BG_COLOR
#define ROM_UI_BAR_COLOR      DMG_RGB565_BAR_COLOR
#define ROM_UI_BORDER_COLOR   DMG_RGB565_BORDER_COLOR
#define ROM_UI_TEXT_COLOR     DMG_RGB565_TEXT_COLOR
#define ROM_UI_TEXT_BAR_COLOR DMG_RGB565_TEXT_BAR_COLOR
#define ROM_UI_SEL_COLOR      DMG_RGB565_SEL_COLOR
#define ROM_UI_ACCENT_COLOR   DMG_RGB565_SEL_COLOR
#define ROM_UI_STATUS_OK      DMG_RGB565_STATUS_OK
#define ROM_UI_STATUS_ERR     DMG_RGB565_STATUS_ERR

#define init_ui_buttons                  dmg_init_ui_buttons
#define read_ui_buttons                  dmg_read_ui_buttons
#define wait_for_all_ui_buttons_released dmg_wait_for_all_buttons_released
#define get_sanitized_basename           dmg_sanitize_basename
#define draw_clean_light_top_bar(t, c, p) dmg_draw_top_bar(&s_lynx_desc, t, "")
#define draw_clean_light_bottom_bar(tf)   dmg_draw_bottom_bar(&s_lynx_desc, "", "")

static void set_backlight_brightness(uint8_t level) {
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    persistent_brightness_level = level;
    if (level >= 5) {
        gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_SIO);
        gpio_set_dir(ILI9341_PIN_BL, GPIO_OUT);
        gpio_put(ILI9341_PIN_BL, 1);
    } else {
        gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_PWM);
        uint slice_num = pwm_gpio_to_slice_num(ILI9341_PIN_BL);
        pwm_config config = pwm_get_default_config();
        pwm_config_set_clkdiv(&config, 4.0f);
        pwm_config_set_wrap(&config, 1000);
        pwm_init(slice_num, &config, true);
        static const uint16_t duty_table[6] = { 0, 200, 400, 600, 800, 1000 };
        pwm_set_gpio_level(ILI9341_PIN_BL, duty_table[level]);
    }
}

static void set_sound_volume_level(uint8_t level) {
    if (level > 5) level = 5;
    persistent_volume_level = level;
    if (level == 0) {
        i2s_volume(&i2s_config, 16);
    } else {
        i2s_volume(&i2s_config, (uint8_t)(5 - level));
    }
}

static void ensure_save_directories(void) {
    f_mkdir("0:/saves");
    f_mkdir(SAVES_DIR);
    f_mkdir("/saves");
    f_mkdir(SAVES_DIR_FALLBACK);
}

extern dmg_emulator_desc_t s_lynx_desc;
#define save_emulator_config() lynx_save_config()
#define load_emulator_config() lynx_load_config()


typedef dmg_action_t ingame_action_e;
#define INGAME_ACTION_RESUME DMG_ACTION_RESUME
#define INGAME_ACTION_RESET  DMG_ACTION_RESET
#define INGAME_ACTION_EXIT   DMG_ACTION_EXIT

static const char *s_lynx_candidate_dirs[] = {
    "0:/roms/lynx",
    "/roms/lynx",
    "roms/lynx",
    "0:/roms/lnx",
    "/roms/lnx",
    "roms/lnx",
    "0:/ROMS/LYNX",
    "/ROMS/LYNX",
    "ROMS/LYNX",
    NULL
};

static bool is_lynx_rom(const char *name) {
    if (!name || name[0] == '\0') return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".lnx") == 0 ||
            strcasecmp(dot, ".lyx") == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".com") == 0 ||
            strcasecmp(dot, ".o") == 0);
}

static bool lynx_save_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    ensure_save_directories();
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));
    FIL fd;
    if (f_open(&fd, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    bool ok = lynx->ContextSave(&fd);
    f_close(&fd);
    return ok;
}

static bool lynx_load_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));
    FIL fd;
    if (f_open(&fd, path, FA_READ) != FR_OK) return false;
    bool ok = lynx->ContextLoad(&fd);
    f_close(&fd);
    return ok;
}

static void lynx_apply_scale(dmg_scale_mode_t mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void lynx_apply_effect(bool effect) {
    persistent_grid_effect = effect;
    ili9341_set_grid_effect(persistent_grid_effect);
}

static void lynx_apply_interleave(bool interleave) {
    persistent_interleave = interleave;
    ili9341_set_interleave(persistent_interleave);
}

static void lynx_apply_filter(bool filter) {
    persistent_shimmerless_enabled = filter;
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

static void lynx_apply_speed(bool turbo) {
    persistent_turbo_mode = turbo;
}

static void lynx_apply_volume(uint8_t vol) {
    set_sound_volume_level(vol);
}

static void lynx_apply_brightness(uint8_t br) {
    set_backlight_brightness(br);
}

/* Combo hooks, defined after the descriptor they are stored in. */
static void lynx_save_config(void);
static bool lynx_get_fps(void);
static void lynx_apply_fps(bool on);

static bool lynx_on_load_rom(const struct dmg_emulator_desc *desc, const char *filename) {
    (void)desc;
    (void)filename;
    const char *dir = dmg_get_current_rom_dir();
    if (dir && dir[0]) {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dir);
    }
    return true;
}

static void draw_lynx_image_row(uint8_t idx, bool selected, ili9341_scale_mode_t scale, bool grid, bool shimmerless, bool interleave) {
    if (idx >= 4) return;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? DMG_RGB565_SEL_COLOR : DMG_RGB565_BG_COLOR;

    ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    char buf[44];
    buf[0] = '\0';

    if (idx == 0) {
        const char *scale_names[] = { "< Fit >", "< Stretch >", "< Integer >" };
        snprintf(buf, sizeof(buf), "Scale:        %s", scale_names[scale]);
    } else if (idx == 1) {
        snprintf(buf, sizeof(buf), "Effect:       %s", grid ? "< Grid >" : "< None >");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Filter:       %s", shimmerless ? "< Shimmerless >" : "< Sharp >");
    } else if (idx == 3) {
        snprintf(buf, sizeof(buf), "Screen:       %s", interleave ? "< Interleave >" : "< Progressive >");
    }

    ili9341_text(buf, wx + 12, (uint16_t)(y + 7u), selected ? DMG_RGB565_TEXT_SEL_COLOR : DMG_RGB565_TEXT_COLOR, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void lynx_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;

    ili9341_scale_mode_t sel_scale = persistent_scale_mode;
    bool sel_grid = persistent_grid_effect;
    bool sel_shimmerless = persistent_shimmerless_enabled;
    bool sel_interleave = persistent_interleave;

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    dmg_wait_for_all_buttons_released();

    if (dmg_is_gb_mode()) {
        ili9341_fill_screen(0x0000);
        ili9341_fill_rect(wx, 0, ww, 240, DMG_RGB565_BG_COLOR);
    } else {
        ili9341_fill_screen(DMG_RGB565_BG_COLOR);
    }
    dmg_draw_top_bar(desc, APP_TITLE, "Image Settings");
    dmg_draw_game_bar(desc, game_name);
    dmg_draw_bottom_bar(desc, "L/R: Adjust", "B: Apply & Back");

    for (uint8_t i = 0; i < 4; i++) {
        draw_lynx_image_row(i, i == cursor, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
    }
    for (uint8_t i = 4; i < 8; i++) {
        uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)i * DMG_UI_ITEM_HEIGHT);
        ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, DMG_RGB565_BG_COLOR);
    }
    ili9341_flush(true);

    while (true) {
        watchdog_update();
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

        if (pushed_up) {
            uint8_t prev = cursor;
            cursor = (cursor == 0) ? 3 : (cursor - 1);
            draw_lynx_image_row(prev, false, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
            draw_lynx_image_row(cursor, true, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == 3) ? 0 : (cursor + 1);
            draw_lynx_image_row(prev, false, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
            draw_lynx_image_row(cursor, true, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
        }

        if (pushed_left || pushed_right || pushed_a) {
            if (cursor == 0) {
                if (pushed_right || pushed_a) {
                    sel_scale = (ili9341_scale_mode_t)((sel_scale + 1) % 3);
                } else {
                    sel_scale = (ili9341_scale_mode_t)((sel_scale == 0) ? 2 : (sel_scale - 1));
                }
                draw_lynx_image_row(0, true, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
            } else if (cursor == 1) {
                sel_grid = !sel_grid;
                draw_lynx_image_row(1, true, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
            } else if (cursor == 2) {
                sel_shimmerless = !sel_shimmerless;
                draw_lynx_image_row(2, true, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
            } else if (cursor == 3) {
                sel_interleave = !sel_interleave;
                draw_lynx_image_row(3, true, sel_scale, sel_grid, sel_shimmerless, sel_interleave);
            }
        }

        if (pushed_b) {
            persistent_scale_mode = sel_scale;
            persistent_grid_effect = sel_grid;
            persistent_shimmerless_enabled = sel_shimmerless;
            persistent_interleave = sel_interleave;
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_grid_effect(persistent_grid_effect);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_set_interleave(persistent_interleave);
            dmg_config_save(desc);
            dmg_wait_for_all_buttons_released();
            return;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}

/* Not const: load_cart_rom_file() sets loading_status per path so the shared
 * loading screen reports "Loading into PSRAM..." on the Core2350B and
 * "Loading into Flash..." on the ILI9341 board. */
/* Resume intro chime (dmg_boot_intro.c, core 1). The intro produces 44.1 kHz;
 * every other sample is kept and a full frame goes to i2s_dma_write(), which
 * blocks on the previous DMA and so paces the stream in real time. */
static int16_t  s_intro_audio[2 * (AUDIO_SAMPLE_RATE / 60)];
static uint32_t s_intro_audio_n = 0;
static uint32_t s_intro_audio_phase = 0;
static int lynx_intro_audio_free(void) { return 64; }
static void lynx_intro_audio_push(int16_t l, int16_t r) {
    if (s_intro_audio_phase++ & 1u) return;
    s_intro_audio[2u * s_intro_audio_n]      = l;
    s_intro_audio[2u * s_intro_audio_n + 1u] = r;
    if (++s_intro_audio_n == AUDIO_SAMPLE_RATE / 60) {
        i2s_dma_write(&i2s_config, s_intro_audio);
        s_intro_audio_n = 0;
    }
}

/* After the resume intro: let its last frame finish so the game's first
 * i2s_dma_write() starts from an idle DMA, and drop any partial frame. */
static void lynx_audio_restore(void) {
    dma_channel_wait_for_finish_blocking(i2s_config.dma_channel);
    s_intro_audio_n = 0;
    s_intro_audio_phase = 0;
}

/* EEPROM to the SD before a standby (dmg_power_off_with_resume). */
static void lynx_flush_saves(void) {
    if (lynx) lynx->SaveEEPROM();
}

dmg_emulator_desc_t s_lynx_desc = {
    .app_title            = APP_TITLE,
    .default_rom_dir      = ROMS_DIR,
    .candidate_dirs       = s_lynx_candidate_dirs,
    .config_path          = CONFIG_PATH,
    .config_fallback      = CONFIG_PATH_FALLBACK,
    .saves_dir            = SAVES_DIR,
    .config_magic         = CONFIG_MAGIC,
    .is_rgb555            = false,
    .display = {
        .fill_rect        = ili9341_fill_rect,
        .draw_text        = ili9341_text,
        .flush_rows       = ili9341_flush_rows,
        .fill_screen      = ili9341_fill_screen,
        .flush            = ili9341_flush,
        .stop_blitter     = ili9341_blitter_stop,
        .start_blitter    = ili9341_blitter_start,
        .write_rect_565   = ili9341_write_rect_565
    },
    .is_rom_file          = is_lynx_rom,
    .on_load_rom          = NULL,
    .save_state           = lynx_save_state,
    .load_state           = lynx_load_state,
    .apply_scale          = lynx_apply_scale,
    .apply_effect         = lynx_apply_effect,
    .apply_interleave     = lynx_apply_interleave,
    .apply_speed          = lynx_apply_speed,
    .apply_volume         = lynx_apply_volume,
    .apply_brightness     = lynx_apply_brightness,
    .apply_filter         = lynx_apply_filter,
    .reset_game           = NULL,
    .exit_game            = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu       = lynx_image_submenu,
    .save_config          = lynx_save_config,
    .get_fps              = lynx_get_fps,
    .apply_fps            = lynx_apply_fps,
    /* The Lynx I2S driver takes whole 22.05 kHz frames, not a sample ring:
     * the intro's 44.1 kHz stream is decimated and sent a frame at a time. */
    .audio_free           = lynx_intro_audio_free,
    .audio_push           = lynx_intro_audio_push,
    .flush_saves          = lynx_flush_saves,
    .audio_restore        = lynx_audio_restore
};

/* Persists this emulator's settings.
 *
 * The emulator keeps them in its own persistent_* variables, while
 * dmg_config_save() writes g_dmg_config -- two copies of the same state. The
 * shared in-game menu writes g_dmg_config first and lets the descriptor callback
 * push it down, so its rows (Speed, Volume, Brightness) saved correctly. The
 * custom "Image >" submenu went the other way, changing persistent_* and then
 * calling save, which wrote whatever g_dmg_config happened to hold. The file was
 * therefore correct on disk but stale by one change, and every Scale / Effect /
 * Screen edit came back at the previous value.
 *
 * Copying them across first is what makes the save mean anything. This is the
 * shape save_config() already had in PicoWS and save_emulator_config() in
 * PicoGBC; the other emulators just never had one. */
static void lynx_save_config(void) {
    g_dmg_config.scale_mode       = (uint32_t)persistent_scale_mode;
    g_dmg_config.grid_effect      = persistent_grid_effect ? 1u : 0u;
    g_dmg_config.interleave       = persistent_interleave ? 1u : 0u;
    g_dmg_config.turbo_mode       = persistent_turbo_mode;
    g_dmg_config.volume_level     = persistent_volume_level;
    g_dmg_config.brightness_level = persistent_brightness_level;
    g_dmg_config.save_slot        = persistent_save_slot;
    g_dmg_config.load_slot        = persistent_load_slot;
    g_dmg_config.extra_opt2       = persistent_shimmerless_enabled ? 0u : 1u;
    dmg_config_save(&s_lynx_desc);
}

static void lynx_load_config(void) {
    /* dmg_config_load() pushes scale/effect/interleave/speed/volume/brightness
     * back into the persistent_* variables through the descriptor callbacks.
     * extra_opt1 has no callback of its own, so the FPS overlay is read and
     * applied by hand. */
    dmg_config_load(&s_lynx_desc);
}


static bool lynx_get_fps(void) {
    return s_fps_overlay_enabled;
}

static void lynx_apply_fps(bool on) {
    s_fps_overlay_enabled = on;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)s_fps_value);
    ili9341_set_fps_overlay(s_fps_overlay_enabled, buf);
}

static bool rom_file_selector(char *selected_out, size_t max_len) {
    return dmg_rom_selector(&s_lynx_desc, selected_out, max_len);
}

static ingame_action_e show_ingame_menu(const char *game_name) {
    return dmg_ingame_menu(&s_lynx_desc, game_name);
}

/* =========================================================================
 * Flash Caching & ROM Loading
 * ========================================================================= */
static FIL s_io_fil_ld;

/* Reads the open cart straight from the SD card into PSRAM, bypassing the Flash
 * XIP window entirely. Mirrors nes_load_rom_to_psram()/pce_load_rom_to_psram().
 *
 * Why: the XIP path erases and re-programs megabytes of QSPI on every load,
 * which is what the 6 s hardware watchdog cannot survive for the larger carts,
 * and it burns a flash erase cycle per game selection. Reading SD -> PSRAM is
 * just a memcpy as far as the flash is concerned.
 *
 * The image is byte-identical to the flash path; the Lynx core addresses
 * s_rom_ptr purely by pointer arithmetic over the *emulated* address, so it
 * never sees a host address. */
static bool lynx_load_rom_to_psram(uint32_t size) {
    /* lwmem returns NULL on exhaustion rather than panicking, so check first. */
    const size_t psram_total = PicoPlusPsram::getInstance().GetMemorySize();
    if (psram_total == 0) {
        printf("[E] PSRAM reports zero usable bytes\n");
        return false;
    }
    /* 512 KB headroom: the CSystem allocates ~76 KB with `new` after this. */
    if ((size_t)size + (512u * 1024u) > psram_total) {
        printf("[E] ROM %u B does not fit in PSRAM (%u B usable)\n",
               (unsigned)size, (unsigned)psram_total);
        return false;
    }

    /* Free the previous game's buffer first -- otherwise switching games leaks
     * every ROM in turn until the allocator is dry. */
    if (s_rom_in_psram && s_rom_ptr) {
        PicoPlusPsram::getInstance().Free(s_rom_ptr);
        s_rom_ptr = (uint8_t *)rom_flash_xip;
        s_rom_in_psram = false;
    }

    uint8_t *buf = (uint8_t *)PicoPlusPsram::getInstance().Malloc(size);
    if (!buf) {
        printf("[E] PSRAM allocation of %u B failed\n", (unsigned)size);
        return false;
    }

    /* Publish before reading, so a short read is still recognisable. */
    s_rom_ptr = buf;
    s_rom_in_psram = true;

    uint32_t offset = 0;
    while (offset < size) {
        watchdog_update();
        uint32_t chunk = (size - offset > 16384u) ? 16384u : (size - offset);
        UINT read = 0;
        if (f_read(&s_io_fil_ld, buf + offset, chunk, &read) != FR_OK || read != chunk) {
            printf("[E] SD read error at offset %u (%u of %u)\n",
                   (unsigned)offset, (unsigned)read, (unsigned)chunk);
            PicoPlusPsram::getInstance().Free(buf);
            s_rom_ptr = (uint8_t *)rom_flash_xip;
            s_rom_in_psram = false;
            return false;
        }
        offset += chunk;

        dmg_update_loading_progress(&s_lynx_desc, offset, size);
    }

    printf("[P] Cart in PSRAM: %u B at %p\n", (unsigned)size, (void *)buf);
    return true;
}

static bool load_cart_rom_file(const char *filename) {
    apply_safe_qmi_timing();
    char path[FF_MAX_LFN + 32];
    const char *curr_dir = dmg_get_current_rom_dir();
    if (curr_dir && curr_dir[0] != '\0') {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", curr_dir);
    }
    snprintf(path, sizeof(path), "%s/%s", current_rom_dir, filename);
    FRESULT fr = f_open(&s_io_fil_ld, path, FA_READ);
    if (fr != FR_OK) {
        snprintf(path, sizeof(path), "%s/%s", ROMS_DIR_FALLBACK, filename);
        fr = f_open(&s_io_fil_ld, path, FA_READ);
    }
    if (fr != FR_OK) {
        printf("[E] Cannot open ROM file: %s\n", filename);
        return false;
    }

    FSIZE_t fsz = f_size(&s_io_fil_ld);
    if (fsz < 512 || fsz > LYNK_ROM_CACHE_MAX) {
        printf("[E] Invalid ROM size: %lu\n", (unsigned long)fsz);
        f_close(&s_io_fil_ld);
        return false;
    }
    uint32_t size = (uint32_t)fsz;
    uint32_t erase_size = (size + FLASH_SECTOR_SIZE - 1) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);

    const bool use_psram = (PicoPlusPsram::getInstance().GetMemorySize() > 0);

    /* Shared Clean Light loading screen, same as the other emulators.
     *
     * This used to be drawn by hand here against a flat 320x240 framebuffer, and
     * it was the only screen in the project that ignored the active viewport: the
     * name started at x=40, i.e. inside the 45px DMG bezel, and the driver's clip
     * only guards ILI9341_WIDTH=320, never the 311 of GB_MODE=1 -- so glyphs ran
     * out past the right margin, which is supposed to be black.
     *
     * dmg_draw_loading_screen() measures the frame from dmg_get_window_x()/w() and
     * wraps the name over two lines, so the bezel and the card border are
     * respected in both modes. The loading logic below is untouched. */
    char name_disp[44];
    get_sanitized_basename(filename, name_disp, sizeof(name_disp));
    for (char *p = name_disp; *p != '\0'; p++) if (*p == '_') *p = ' ';

    s_lynx_desc.loading_status = use_psram ? "Loading into PSRAM..." : "Loading into Flash...";
    dmg_draw_loading_screen(&s_lynx_desc, "LOADING LYNX CART", name_disp, size);

    if (use_psram) {
        /* Does not touch QMI M0 timing: it reads via FatFs on the SD bus and
         * writes to the PSRAM on the QSPI bus. apply_safe_qmi_timing() is a
         * Flash-XIP divider fix and has no business here. */
        if (!lynx_load_rom_to_psram(size)) {
            f_close(&s_io_fil_ld);
            return false;
        }
        f_close(&s_io_fil_ld);

        strncpy(selectedRom, filename, sizeof(selectedRom) - 1);
        selectedRom[sizeof(selectedRom) - 1] = '\0';
        rom_size = size;
        return true;
    }

    auto flash_target_offset = FLASH_TARGET_OFFSET;

    /* Same cart already in flash? Compare before erasing: on a standby resume
     * it always is, and rewriting it would freeze the intro for seconds. Stops
     * at the first differing block for another cart. */
    {
        uint8_t cmp_block[4096];
        uint32_t cmp_off = 0;
        bool same = true;
        while (cmp_off < size) {
            watchdog_update();
            uint32_t chunk = (size - cmp_off > sizeof(cmp_block)) ? sizeof(cmp_block) : (size - cmp_off);
            UINT read = 0;
            if (f_read(&s_io_fil_ld, cmp_block, chunk, &read) != FR_OK || read != chunk ||
                memcmp(cmp_block, (const uint8_t *)rom_flash_xip + cmp_off, chunk) != 0) {
                same = false;
                break;
            }
            cmp_off += chunk;
        }
        f_lseek(&s_io_fil_ld, 0);
        if (same) {
            printf("[I] Cart already in flash, skipping write\n");
            f_close(&s_io_fil_ld);
            if (s_rom_in_psram && s_rom_ptr) {
                PicoPlusPsram::getInstance().Free(s_rom_ptr);
                s_rom_in_psram = false;
            }
            s_rom_ptr = (uint8_t *)rom_flash_xip;
            strncpy(selectedRom, filename, sizeof(selectedRom) - 1);
            selectedRom[sizeof(selectedRom) - 1] = '\0';
            rom_size = size;
            return true;
        }
    }

    dmg_boot_intro_flash_begin();
    safe_flash_range_erase(flash_target_offset, erase_size);

    uint8_t copy_block[4096];
    uint32_t offset = 0;

    while (offset < size) {
        watchdog_update();
        uint32_t chunk = (size - offset > sizeof(copy_block)) ? sizeof(copy_block) : (size - offset);
        UINT read = 0;
        if (f_read(&s_io_fil_ld, copy_block, chunk, &read) != FR_OK || read != chunk) {
            printf("[E] SD read error at offset %u\n", (unsigned)offset);
            f_close(&s_io_fil_ld);
            dmg_boot_intro_flash_end();
            return false;
        }

        const uint32_t programmed = (chunk + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
        if (programmed > chunk) memset(copy_block + chunk, 0xFF, programmed - chunk);

        for (uint32_t pos = 0; pos < programmed; pos += FLASH_PAGE_SIZE) {
            safe_flash_range_program(flash_target_offset + offset + pos, copy_block + pos, FLASH_PAGE_SIZE);
        }

        offset += chunk;

        dmg_update_loading_progress(&s_lynx_desc, offset, size);
    }

    f_close(&s_io_fil_ld);
    dmg_boot_intro_flash_end();
    apply_safe_qmi_timing();

    /* Flash path: any PSRAM buffer from a previous game is released here, so
     * the two paths cannot leave a dangling s_rom_ptr behind. */
    if (s_rom_in_psram && s_rom_ptr) {
        PicoPlusPsram::getInstance().Free(s_rom_ptr);
        s_rom_in_psram = false;
    }
    s_rom_ptr = (uint8_t *)rom_flash_xip;

    strncpy(selectedRom, filename, sizeof(selectedRom) - 1);
    selectedRom[sizeof(selectedRom) - 1] = '\0';
    rom_size = size;
    return true;
}

/* =========================================================================
 * Joypad Input Mapping & Combined In-Game Controls
 * ========================================================================= */
static bool s_btn_up = false, s_btn_down = false, s_btn_left = false, s_btn_right = false;
static bool s_btn_a = false, s_btn_b = false, s_btn_start = false;
static bool s_btn_select = false;

static void update_ngp_input(void) {
    dmg_pad_t pad_in;
    read_ui_buttons(&pad_in.up, &pad_in.down, &pad_in.left, &pad_in.right,
                    &pad_in.a, &pad_in.b, &pad_in.select, &pad_in.start);

    /* Combos: SELECT+START exits (held ~1 s), START+B pauses, START+A toggles
     * the FPS overlay, SELECT+A toggles 1x/2x, SELECT+UP/DOWN is the volume.
     * The pad comes back already masked, so a combo never reaches the Lynx as a
     * stray press. */
    dmg_combo_events_t ev;
    dmg_combo_frame(&pad_in, &s_lynx_desc, &ev);

    if (ev.request_pause_menu) s_request_pause_menu = true;
    if (ev.request_exit_game)  s_request_exit_game  = true;
    if (ev.request_power_off) {
        s_request_power_off = true;
        s_power_reason = ev.power_reason;
    }
    if (ev.speed_toggled) {
        printf("[I] Combo SELECT+A: speed = %s\n",
               persistent_turbo_mode ? "2x" : "1x");
    }

    s_btn_up = pad_in.up;
    s_btn_down = pad_in.down;
    s_btn_left = pad_in.left;
    s_btn_right = pad_in.right;
    s_btn_a = pad_in.a;
    s_btn_b = pad_in.b;
    s_btn_start = pad_in.start;
    s_btn_select = pad_in.select;
}

/* =========================================================================
 * Gameplay Session Loop
 * ========================================================================= */
static void run_game(void) {
    apply_safe_qmi_timing();
    lynx = new CSystem(s_rom_ptr, rom_size, MIKIE_PIXEL_FORMAT_16BPP_565, HANDY_AUDIO_SAMPLE_FREQ);

    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_grid_effect(persistent_grid_effect);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_set_interleave(persistent_interleave);
    s_request_pause_menu = false;
    s_request_exit_game = false;
    reset_requested = false;
    /* Drop any half-finished SELECT+START hold or closed edge latch left over
     * from the previous game. */
    dmg_combo_reset();

    dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */

    ili9341_clear_black();
    ili9341_blitter_start();
    watchdog_enable(6000, false);

    uint64_t frame_start_us = time_us_64();
    uint64_t frame_budget = persistent_turbo_mode ? 8333u : 16666u;
    uint32_t fps_frames = 0;
    uint64_t fps_window_us = frame_start_us;

    while (!s_request_exit_game) {
        watchdog_update();

        if (reset_requested) {
            lynx->Reset();
            reset_requested = false;
        }

        update_ngp_input();

        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (s_request_power_off) {
            s_request_power_off = false;
            ili9341_blitter_stop();
            dmg_power_off_with_resume(&s_lynx_desc, (dmg_power_reason_t)s_power_reason);
        }

        if (s_request_pause_menu) {
            s_request_pause_menu = false;
            ingame_action_e action = show_ingame_menu(selectedRom);
            if (action == INGAME_ACTION_EXIT) {
                s_request_exit_game = true;
                break;
            }
            ili9341_clear_black();
            ili9341_blitter_start();
            if (action == INGAME_ACTION_RESET) {
                lynx->Reset();
                reset_requested = false;
            }
            frame_start_us = time_us_64();
            continue;
        }

        uint8_t buttons = 0;
        if (s_btn_left)  buttons |= BUTTON_LEFT;
        if (s_btn_right) buttons |= BUTTON_RIGHT;
        if (s_btn_up)    buttons |= BUTTON_UP;
        if (s_btn_down)  buttons |= BUTTON_DOWN;
        if (s_btn_a)     buttons |= BUTTON_A;
        if (s_btn_b)     buttons |= BUTTON_B;
        if (s_btn_start) buttons |= BUTTON_OPT2;
        if (s_btn_select) buttons |= BUTTON_OPT1;

        lynx->SetButtonData(buttons);

        lynx->UpdateFrame(true);

        const uint8_t *frame = (const uint8_t *)gPrimaryFrameBuffer;
        for (int l = 0; l < HANDY_SCREEN_HEIGHT; l++) {
            if (ili9341_is_active_lynx_line(l)) {
                ili9341_stream_lynx_line(l, frame + l * HANDY_SCREEN_WIDTH, lynx_palette_rgb565);
            }
        }
        ili9341_stream_lynx_frame_end();

        static int16_t dma_buffer[AUDIO_BUFFER_LENGTH];
        memcpy(dma_buffer, gAudioBuffer, AUDIO_BUFFER_LENGTH * 2);
        i2s_dma_write(&i2s_config, (const int16_t *)dma_buffer);

        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            ili9341_blitter_stop();
            const bool resumed = lynx_load_state(selectedRom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_clear_black();
            ili9341_blitter_start();
            frame_start_us = time_us_64();
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }

        fps_frames++;
        uint64_t now_us = time_us_64();
        if ((now_us - fps_window_us) >= 1000000u) {
            s_fps_value = (uint32_t)(((uint64_t)fps_frames * 1000000u) / (now_us - fps_window_us));
            if (s_fps_overlay_enabled) {
                char fps_buf[8];
                snprintf(fps_buf, sizeof(fps_buf), "%02u", (unsigned)s_fps_value);
                ili9341_set_fps_overlay(true, fps_buf);
            }
            fps_frames = 0;
            fps_window_us = now_us;
        }
        uint64_t target_next = frame_start_us + frame_budget;
        if ((int64_t)(target_next - now_us) > 0) {
            busy_wait_us_32((uint32_t)(target_next - now_us));
        }
        frame_start_us = time_us_64();
    }

    watchdog_disable();
    lynx->SaveEEPROM();
    delete lynx;
    lynx = nullptr;
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */
int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    set_sys_clock_khz(360000, true);
    apply_safe_qmi_timing();

    /* Decouple SPI peripheral clock from the CPU clock. With clk_peri following
     * clk_sys (360 MHz) the PL022 divider cannot produce 72 MHz and clamps to
     * 60 MHz, starving the ILI9341 stream. Route clk_peri to PLL_USB at 144 MHz
     * (VCO 1152 MHz, postdiv 4*2 = 8) so divisor 2 gives an exact 72 MHz. */
    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
        clock_get_hz(clk_sys),
        clock_get_hz(clk_sys)
    );
    pll_init(pll_usb, 1, 840 * MHZ, 6, 1);
    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
        140 * MHZ,
        140 * MHZ
    );

    stdio_init_all();
    init_ui_buttons();

    ili9341_init();

    uint32_t mount_attempts = 0;
    while (f_mount(&fs, "0:", 1) != FR_OK && f_mount(&fs, "", 1) != FR_OK) {
        watchdog_update();
        if (++mount_attempts >= 50) {
            printf("[E] PicoLINX: SD mount failed after 50 attempts\n");
            break; // no SD card fitted or mount failure - keep the menu drawing
        }
        sleep_ms(50);
    }

    dmg_init_boot_mode();

    i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLE_RATE / 60;
    i2s_volume(&i2s_config, 2);
    i2s_init(&i2s_config);

    load_emulator_config();
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_grid_effect(persistent_grid_effect);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_set_interleave(persistent_interleave);
    set_backlight_brightness(persistent_brightness_level);
    set_sound_volume_level(persistent_volume_level);

    gAudioBuffer = (uint16_t *)audio_buffer;
    gAudioEnabled = true;
    gPrimaryFrameBuffer = (uint8_t *)SCREEN;

    for (int i = 0; i < 256; i++) {
        uint8_t r8 = (((i & 0xe0) >> 4) * 16);
        uint8_t g8 = (((i & 0x1C) >> 1) * 16);
        uint8_t b8 = (((i & 0x03) << 2) * 16);
        uint16_t r5 = r8 >> 3;
        uint16_t g6 = g8 >> 2;
        uint16_t b5 = b8 >> 3;
        lynx_palette_rgb565[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }

    ensure_save_directories();

    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_lynx_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_lynx_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_lynx_desc);
            snprintf(selectedRom, sizeof(selectedRom), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_lynx_desc)) dmg_boot_intro_finish();
        }
    }

    while (true) {
        if (s_resume_pending || rom_file_selector(selectedRom, sizeof(selectedRom))) {
            if (load_cart_rom_file(selectedRom)) {
                /* The config file is the source of truth for how the game is
                 * displayed, so it is re-read when a game is entered, not only at
                 * boot. Safe to repeat because every menu visit commits first, so
                 * the file always matches what was on screen when the menu closed.
                 */
                load_emulator_config();
                ili9341_set_scale_mode(persistent_scale_mode);
                ili9341_set_grid_effect(persistent_grid_effect);
                ili9341_set_shimmerless(persistent_shimmerless_enabled);
                ili9341_set_interleave(persistent_interleave);
                set_backlight_brightness(persistent_brightness_level);
                set_sound_volume_level(persistent_volume_level);
                dmg_power_set_game(selectedRom, selectedRom);
                run_game();
                dmg_power_set_game(NULL, NULL);
            } else {
                if (s_resume_pending) {
                    s_resume_pending = false;
                    dmg_resume_consume();
                }
                dmg_boot_intro_finish();
                sleep_ms(1500);
            }
        }
    }
    __unreachable();
}
