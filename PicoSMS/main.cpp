/**
 * PicoSMS & PicoGG for Raspberry Pi RP2350 + ILI9341 SPI TFT + MAX98357A I2S
 *
 * Clean Light UI Theme, In-Game Pause Menu, Save/Load States,
 * Scanlines & Grid Effects, Dedicated SD Card Folders.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <algorithm>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/divider.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/pwm.h"

#include "ff.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "external_audio.h"
#include "state.h"

#if USE_ILI9341
#include "ili9341.h"
#endif

#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"

/* SmsPlus core headers */
#include "shared.h"

/* =========================================================================
 * Build Target Configuration (PicoSMS vs PicoGG)
 * ========================================================================= */
#ifndef SMS_IS_GG_BUILD
#define SMS_IS_GG_BUILD 0
#endif

#if SMS_IS_GG_BUILD
#define APP_TITLE            "PicoGG"
#define CONFIG_MAGIC         0x47475350u /* "PSGG" */
#define CONFIG_PATH          "0:/saves/gg/picogg.cfg"
#define CONFIG_PATH_FALLBACK "/saves/gg/picogg.cfg"
#define SAVES_DIR            "0:/saves/gg"
#define SAVES_DIR_FALLBACK   "/saves/gg"
#define ROMS_DIR             "0:/roms/gg"
#define ROMS_DIR_FALLBACK    "/roms/gg"
#define TARGET_CART_TYPE     TYPE_GG
#else
#define APP_TITLE            "PicoSMS"
#define CONFIG_MAGIC         0x534D5350u /* "PSMS" */
#define CONFIG_PATH          "0:/saves/sms/picosms.cfg"
#define CONFIG_PATH_FALLBACK "0:/saves/ms/picosms.cfg"
#define SAVES_DIR            "0:/saves/sms"
#define SAVES_DIR_FALLBACK   "0:/saves/ms"
#define ROMS_DIR             "0:/roms/sms"
#define ROMS_DIR_FALLBACK    "0:/roms/ms"
#define TARGET_CART_TYPE     TYPE_SMS
#endif

#define AUDIOBUFFERSIZE 1024
#if PICO_RP2350
#define EMULATOR_CLOCKFREQ_KHZ 302400
#else
#define EMULATOR_CLOCKFREQ_KHZ 252000
#endif
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

static bool isFatalError = false;
static bool isGameGear = (SMS_IS_GG_BUILD != 0);
static char selectedRom[FF_MAX_LFN];
static uint32_t last_flashed_rom_size = 0;

static uint32_t fps = 0;
static char fpsString[8] = "60";

static uint32_t s_audio_batch[1024];
static size_t s_audio_batch_len = 0;

static uint16_t s_sms_palette[32];

/* =========================================================================
 * Persistent Configuration Structure
 * ========================================================================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t scale_mode;        /* 0 = Fit, 1 = Stretch, 2 = Integer */
    uint32_t effect_enabled;    /* 0 = Normal, 1 = Scanlines (SMS) / Grid (GG) */
    uint32_t display_fps;       /* 0 = Off, 1 = On */
    uint32_t volume_level;      /* 1..5, default 3 */
    uint32_t brightness_level;  /* 1..5, default 5 */
    uint32_t turbo_mode;        /* 0 = Normal 1x, 1 = Turbo 2x */
    uint32_t save_slot;         /* 1..4, default 1 */
    uint32_t load_slot;
    uint32_t interleave;         /* 1..4, default 1 */
} emulator_config_t;

static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_effect_enabled = false;
static bool persistent_shimmerless_enabled = true;
static bool persistent_interleave = true;
static bool persistent_display_fps = false;
static uint8_t persistent_volume_level = 3;     /* 1..5 */
static uint8_t persistent_brightness_level = 5; /* 1..5 */
static uint8_t persistent_turbo_mode = 0;       /* 0 = 1x, 1 = 2x */
static uint8_t persistent_save_slot = 1;        /* 1..4 */
static uint8_t persistent_load_slot = 1;        /* 1..4 */

static bool s_request_pause_menu = false;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;
static bool s_request_exit_game = false;
static bool reset = false;

/* =========================================================================
 * UI & Button Aliases via shared_menu
 * ========================================================================= */
#define init_ui_buttons                  dmg_init_ui_buttons
#define read_ui_buttons                  dmg_read_ui_buttons
#define wait_for_all_ui_buttons_released dmg_wait_for_all_buttons_released

/* =========================================================================
 * Display Backlight & Sound Volume Controls
 * ========================================================================= */
static void set_backlight_brightness(uint8_t level) {
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    persistent_brightness_level = level;
#if USE_ILI9341
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
#endif
}

static void set_sound_volume_level(uint8_t level) {
    if (level > 5) level = 5;
    persistent_volume_level = level;
}

static void ensure_save_directories(void) {
    f_mkdir("0:/saves");
    f_mkdir(SAVES_DIR);
}

#define get_sanitized_basename dmg_sanitize_basename
#define build_state_path(g, s, out, len) dmg_build_state_path(SAVES_DIR, g, s, out, len)

extern "C" void system_load_sram(void) {
    if (!sms.sram) return;
    char base[64];
    get_sanitized_basename(selectedRom, base, sizeof(base));

    char sram_path[FF_MAX_LFN];
    snprintf(sram_path, sizeof(sram_path), "%s/%s.sav", SAVES_DIR, base);
    FIL fil;
    FRESULT fr = f_open(&fil, sram_path, FA_READ);
    if (fr != FR_OK) {
        snprintf(sram_path, sizeof(sram_path), "%s/%s.sav", SAVES_DIR_FALLBACK, base);
        fr = f_open(&fil, sram_path, FA_READ);
    }
    if (fr == FR_OK) {
        UINT br = 0;
        f_read(&fil, sms.sram, 0x8000, &br);
        f_close(&fil);
        printf("[I] Battery SRAM loaded (%u bytes) from %s\n", (unsigned)br, sram_path);
    }
}

extern "C" void system_save_sram(void) {
    if (!sms.sram || !sms.save) return;
    ensure_save_directories();

    char base[64];
    get_sanitized_basename(selectedRom, base, sizeof(base));

    char sram_path[FF_MAX_LFN];
    snprintf(sram_path, sizeof(sram_path), "%s/%s.sav", SAVES_DIR, base);
    FIL fil;
    FRESULT fr = f_open(&fil, sram_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        snprintf(sram_path, sizeof(sram_path), "%s/%s.sav", SAVES_DIR_FALLBACK, base);
        fr = f_open(&fil, sram_path, FA_WRITE | FA_CREATE_ALWAYS);
    }
    if (fr == FR_OK) {
        UINT bw = 0;
        f_write(&fil, sms.sram, 0x8000, &bw);
        f_close(&fil);
        printf("[I] Battery SRAM saved (%u bytes) to %s\n", (unsigned)bw, sram_path);
        sms.save = 0;
    }
}

/* =========================================================================
 * Palettes Synchronization (SMS & GG 4+4+4)
 * ========================================================================= */
extern "C" void sms_palette_sync(int index) {
    if (index < 0 || index >= 32) return;
    /* SMS CRAM: 2 bits per channel (0..3) -> 6-bit -> RGB565 */
    const uint32_t c = vdp.cram[index];
    const uint8_t r = (uint8_t)((c >> 0) & 3u);
    const uint8_t g = (uint8_t)((c >> 2) & 3u);
    const uint8_t b = (uint8_t)((c >> 4) & 3u);
    const uint8_t r6 = (uint8_t)((r << 6) | (r << 4) | (r << 2) | r);
    const uint8_t g6 = (uint8_t)((g << 6) | (g << 4) | (g << 2) | g);
    const uint8_t b6 = (uint8_t)((b << 6) | (b << 4) | (b << 2) | b);
    const uint16_t r5 = (uint16_t)(r6 >> 3);
    const uint16_t g6e = (uint16_t)(g6 >> 2);
    const uint16_t b5 = (uint16_t)(b6 >> 3);
    s_sms_palette[index] = (uint16_t)((r5 << 11) | (g6e << 5) | b5);
}

extern "C" void sms_palette_syncGG(int index) {
    if (index < 0 || index >= 32) return;
    /* GG CRAM 12-bit (4+4+4):
     * byte 0 (even) = %GGGGRRRR
     * byte 1 (odd)  = %0000BBBB */
    const uint8_t r4 = (uint8_t)((vdp.cram[(index << 1) | 0] >> 0) & 0x0Fu);
    const uint8_t g4 = (uint8_t)((vdp.cram[(index << 1) | 0] >> 4) & 0x0Fu);
    const uint8_t b4 = (uint8_t)((vdp.cram[(index << 1) | 1] >> 0) & 0x0Fu);
    const uint8_t r8 = (uint8_t)((r4 << 4) | r4);
    const uint8_t g8 = (uint8_t)((g4 << 4) | g4);
    const uint8_t b8 = (uint8_t)((b4 << 4) | b4);
    const uint16_t r5 = (uint16_t)(r8 >> 3);
    const uint16_t g6 = (uint16_t)(g8 >> 2);
    const uint16_t b5 = (uint16_t)(b8 >> 3);
    s_sms_palette[index] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* =========================================================================
 * Scanline Rendering Callback (Streamed to ILI9341 via Core 1)
 * ========================================================================= */
extern "C" void in_ram(sms_render_line)(int line, const uint8_t *buffer) {
    if (!buffer) return;
    /* Sonic the Hedgehog leftmost column blanking: crop 8px when bit 5 is set */
    bool crop_left = (!isGameGear && ((vdp.reg[0] & 0x20u) != 0u));
    ili9341_stream_sms_line(line, buffer, s_sms_palette, crop_left);
}

/* =========================================================================
 * Audio Processing (I2S DMA Batch Streaming to MAX98357A)
 * ========================================================================= */
static void sms_psg_postprocess(int16_t raw_l, int16_t raw_r, int16_t *out_l, int16_t *out_r) {
    static int32_t prev_l = 0, prev_r = 0;
    static int32_t dc_l = 0, dc_r = 0;
    int32_t xl = (int32_t)raw_l;
    int32_t xr = (int32_t)raw_r;
    int32_t yl = xl - prev_l + dc_l - (dc_l >> 9);
    int32_t yr = xr - prev_r + dc_r - (dc_r >> 9);
    prev_l = xl; prev_r = xr;
    dc_l = yl; dc_r = yr;
    yl = yl + (yl >> 1);
    yr = yr + (yr >> 1);
    yl >>= 1;
    yr >>= 1;

    /* Digital volume attenuation (Levels 1..5) */
    static const int32_t vol_table[6] = { 0, 51, 102, 154, 205, 256 };
    yl = (yl * vol_table[persistent_volume_level]) >> 8;
    yr = (yr * vol_table[persistent_volume_level]) >> 8;

    if (yl > 32767) yl = 32767; else if (yl < -32768) yl = -32768;
    if (yr > 32767) yr = 32767; else if (yr < -32768) yr = -32768;
    *out_l = (int16_t)yl;
    *out_r = (int16_t)yr;
}

static void inline processaudioPerFrameI2S(void) {
    if (snd.bufsize <= 0) return;
    int n = snd.bufsize > 1024 ? 1024 : snd.bufsize;
    for (int i = 0; i < n; i++) {
        int16_t l = snd.buffer[0][i];
        int16_t r = snd.buffer[1][i];
        int16_t ol, or_;
        sms_psg_postprocess(l, r, &ol, &or_);
        s_audio_batch[i] = ((uint32_t)(uint16_t)ol << 16) | (uint16_t)or_;
    }
    EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_batch, n);
}

/* =========================================================================
 * Clean Light UI via shared_menu
 * ========================================================================= */
typedef dmg_action_t ingame_action_e;
#define INGAME_ACTION_RESUME DMG_ACTION_RESUME
#define INGAME_ACTION_RESET  DMG_ACTION_RESET
#define INGAME_ACTION_EXIT   DMG_ACTION_EXIT

#if SMS_IS_GG_BUILD
static const char *s_sms_candidate_dirs[] = {
    "0:/roms/gg",
    "/roms/gg",
    "roms/gg",
    "0:/ROMS/GG",
    "/ROMS/GG",
    "ROMS/GG",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};
#else
static const char *s_sms_candidate_dirs[] = {
    "0:/roms/sms",
    "/roms/sms",
    "roms/sms",
    "0:/roms/ms",
    "/roms/ms",
    "roms/ms",
    "0:/ROMS/SMS",
    "/ROMS/SMS",
    "ROMS/SMS",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};
#endif

static bool is_valid_emulator_rom(const char *filename) {
    if (!filename) return false;
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
#if SMS_IS_GG_BUILD
    return (strcasecmp(dot, ".gg") == 0 || strcasecmp(dot, ".bin") == 0);
#else
    return (strcasecmp(dot, ".sms") == 0 || strcasecmp(dot, ".bin") == 0 || strcasecmp(dot, ".sg") == 0);
#endif
}

static bool sms_save_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path(SAVES_DIR, game_name, slot, state_path, sizeof(state_path));
    ensure_save_directories();
    return (Emulator_SaveState(state_path) == 0);
}

static bool sms_load_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path(SAVES_DIR, game_name, slot, state_path, sizeof(state_path));
    int res = Emulator_LoadState(state_path);
    if (res != 0 && slot == 1) {
        char legacy_path[DMG_STATE_PATH_MAX];
        char base[64];
        dmg_sanitize_basename(game_name, base, sizeof(base));
        snprintf(legacy_path, sizeof(legacy_path), "%s/%s.ss", SAVES_DIR, base);
        res = Emulator_LoadState(legacy_path);
        if (res != 0) {
            snprintf(legacy_path, sizeof(legacy_path), "%s/%s.ss", SAVES_DIR_FALLBACK, base);
            res = Emulator_LoadState(legacy_path);
        }
    }
    return (res == 0);
}

/* Slot probe for the shared menu's Load State row.
 *
 * Mirrors sms_load_state() above, path for path and in the same order, because
 * the row is only truthful if it answers "can this be loaded" and not "is there
 * anything in that folder". The generic probe would answer the second question:
 * it stops at the numbered file in SAVES_DIR, and sms_load_state() also reaches
 * the unnumbered legacy name of slot 1, in SAVES_DIR and in SAVES_DIR_FALLBACK
 * ("/saves/ms" on a PicoSMS card, "/saves/gg" on a PicoGG one).
 *
 * Nothing extra is probed on purpose. The numbered file in SAVES_DIR_FALLBACK is
 * deliberately left out: sms_load_state() never looks there, so offering it would
 * be a slot the player can select and cannot load. */
static bool sms_state_exists(const char *game_name, uint8_t slot) {
    if (slot < 1 || slot > 4) return false;

    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path(SAVES_DIR, game_name, slot, state_path, sizeof(state_path));
    if (dmg_state_file_present(state_path)) return true;

    if (slot == 1) {
        char base[64];
        dmg_sanitize_basename(game_name, base, sizeof(base));

        char legacy_path[DMG_STATE_PATH_MAX];
        snprintf(legacy_path, sizeof(legacy_path), "%s/%s.ss", SAVES_DIR, base);
        if (dmg_state_file_present(legacy_path)) return true;

        snprintf(legacy_path, sizeof(legacy_path), "%s/%s.ss", SAVES_DIR_FALLBACK, base);
        if (dmg_state_file_present(legacy_path)) return true;
    }
    return false;
}

static void sms_apply_scale(dmg_scale_mode_t mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void sms_apply_effect(bool effect) {
    persistent_effect_enabled = effect;
#if SMS_IS_GG_BUILD
    ili9341_set_grid(persistent_effect_enabled);
#else
    ili9341_set_scanlines(persistent_effect_enabled);
#endif
}

static void sms_apply_filter(bool shimmerless) {
    persistent_shimmerless_enabled = shimmerless;
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

static void sms_apply_interleave(bool interleave) {
    persistent_interleave = interleave;
    ili9341_set_interleave(persistent_interleave);
}

static void sms_apply_speed(bool turbo) {
    persistent_turbo_mode = turbo ? 1 : 0;
}

static void sms_apply_volume(uint8_t vol) {
    set_sound_volume_level(vol);
}

static void sms_apply_brightness(uint8_t br) {
    set_backlight_brightness(br);
}

/* Combo hooks, defined after the descriptor they are stored in. */
static void sms_save_config(void);
static bool sms_get_fps(void);
static void sms_apply_fps(bool on);

static void draw_sms_image_row(uint8_t idx, bool selected) {
    if (idx >= 5) return;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? DMG_RGB555_SEL_COLOR : DMG_RGB555_BG_COLOR;

    ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    char buf[44];
    buf[0] = '\0';

    if (idx == 0) {
        const char *scale_names[] = { "< Fit >", "< Stretch >", "< Integer >" };
        snprintf(buf, sizeof(buf), "Scale:        %s", scale_names[persistent_scale_mode]);
    } else if (idx == 1) {
#if SMS_IS_GG_BUILD
        snprintf(buf, sizeof(buf), "Effect:       %s", persistent_effect_enabled ? "< Grid >" : "< Normal >");
#else
        snprintf(buf, sizeof(buf), "Effect:       %s", persistent_effect_enabled ? "< Scanlines >" : "< Normal >");
#endif
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Filter:       %s", persistent_shimmerless_enabled ? "< Shimmerless >" : "< Sharp >");
    } else if (idx == 3) {
        snprintf(buf, sizeof(buf), "FPS Overlay:  %s", persistent_display_fps ? "< On >" : "< Off >");
    } else if (idx == 4) {
        snprintf(buf, sizeof(buf), "Screen:       %s", persistent_interleave ? "< Interleave >" : "< Progressive >");
    }

    ili9341_text(buf, (uint16_t)(wx + 12u), (uint16_t)(y + 7u), selected ? DMG_RGB555_TEXT_SEL_COLOR : DMG_RGB555_TEXT_COLOR, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void sms_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0; /* 0: Scale, 1: Effect, 2: Filter, 3: FPS, 4: Interleave */
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    dmg_wait_for_all_buttons_released();

    if (dmg_is_gb_mode()) {
        ili9341_fill_screen(0x0000);
        ili9341_fill_rect(wx, 0, ww, 240, DMG_RGB555_BG_COLOR);
    } else {
        ili9341_fill_screen(DMG_RGB555_BG_COLOR);
    }
    dmg_draw_top_bar(desc, desc->app_title, NULL);
    dmg_draw_game_bar(desc, game_name);
    dmg_draw_bottom_bar(desc, "L/R / A: Adjust", "B: Back");

    for (uint8_t i = 0; i < 5; i++) {
        draw_sms_image_row(i, i == cursor);
    }
    for (uint8_t i = 5; i < 8; i++) {
        uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)i * DMG_UI_ITEM_HEIGHT);
        ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, DMG_RGB555_BG_COLOR);
    }
    ili9341_flush(true);

    while (true) {
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
            cursor = (cursor == 0) ? 4 : (cursor - 1);
            draw_sms_image_row(prev, false);
            draw_sms_image_row(cursor, true);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == 4) ? 0 : (cursor + 1);
            draw_sms_image_row(prev, false);
            draw_sms_image_row(cursor, true);
        }

        if (pushed_left || pushed_right || pushed_a) {
            if (cursor == 0) {
                if (pushed_right || pushed_a) {
                    persistent_scale_mode = (ili9341_scale_mode_t)((persistent_scale_mode + 1) % 3);
                } else {
                    persistent_scale_mode = (ili9341_scale_mode_t)((persistent_scale_mode == 0) ? 2 : (persistent_scale_mode - 1));
                }
                ili9341_set_scale_mode(persistent_scale_mode);
                sms_save_config();
                draw_sms_image_row(0, true);
            } else if (cursor == 1) {
                persistent_effect_enabled = !persistent_effect_enabled;
#if SMS_IS_GG_BUILD
                ili9341_set_grid(persistent_effect_enabled);
#else
                ili9341_set_scanlines(persistent_effect_enabled);
#endif
                sms_save_config();
                draw_sms_image_row(1, true);
            } else if (cursor == 2) {
                persistent_shimmerless_enabled = !persistent_shimmerless_enabled;
                ili9341_set_shimmerless(persistent_shimmerless_enabled);
                sms_save_config();
                draw_sms_image_row(2, true);
            } else if (cursor == 3) {
                persistent_display_fps = !persistent_display_fps;
                ili9341_set_fps_overlay(persistent_display_fps, fpsString);
                sms_save_config();
                draw_sms_image_row(3, true);
            } else if (cursor == 4) {
                persistent_interleave = !persistent_interleave;
                ili9341_set_interleave(persistent_interleave);
                sms_save_config();
                draw_sms_image_row(4, true);
            }
        }

        if (pushed_b) {
            dmg_wait_for_all_buttons_released();
            return;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}

#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
/* Resume intro chime, pushed from core 1 into the I2S ring (dmg_boot_intro.c). */
static int  sms_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void sms_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void sms_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(44100);
}
#endif
/* Battery save to the SD before a standby (dmg_power_off_with_resume). */
static void sms_flush_saves(void) { system_save_sram(); }

static const dmg_emulator_desc_t s_sms_desc = {
    .app_title            = APP_TITLE,
    .default_rom_dir      = ROMS_DIR,
    .candidate_dirs       = s_sms_candidate_dirs,
    .config_path          = CONFIG_PATH,
    .config_fallback      = CONFIG_PATH_FALLBACK,
    .saves_dir            = SAVES_DIR,
    .config_magic         = CONFIG_MAGIC,
    .is_rgb555            = true,
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
    .is_rom_file          = is_valid_emulator_rom,
    .on_load_rom          = NULL,
    .save_state           = sms_save_state,
    .load_state           = sms_load_state,
    .apply_scale          = sms_apply_scale,
    .apply_effect         = sms_apply_effect,
    .apply_interleave     = sms_apply_interleave,
    .apply_speed          = sms_apply_speed,
    .apply_volume         = sms_apply_volume,
    .apply_brightness     = sms_apply_brightness,
    .apply_filter         = sms_apply_filter,
    .reset_game           = NULL,
    .exit_game            = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu       = sms_image_submenu,
    /* On the console build (PSRAM_CS_PIN != 0) the ROM is read straight from the
     * SD card into PSRAM, so the loading screen must not claim to write a flash
     * cache it never touches. The HW15 build has no PSRAM and still programs
     * flash, so there the default wording is correct -- hence the pin test
     * rather than a hardcoded string. */
    .loading_status       = (PSRAM_CS_PIN != 0) ? "Writing PSRAM cache..." : NULL,
    .save_config          = sms_save_config,
    .get_fps              = sms_get_fps,
    .apply_fps            = sms_apply_fps,
    .state_exists         = sms_state_exists,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_free           = sms_audio_free,
    .audio_push           = sms_audio_push,
#endif
    .flush_saves          = sms_flush_saves,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_restore        = sms_audio_restore
#endif
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
static void sms_save_config(void) {
    g_dmg_config.scale_mode       = (uint32_t)persistent_scale_mode;
    g_dmg_config.grid_effect      = persistent_effect_enabled ? 1u : 0u;
    g_dmg_config.interleave       = persistent_interleave ? 1u : 0u;
    g_dmg_config.turbo_mode       = persistent_turbo_mode;
    g_dmg_config.volume_level     = persistent_volume_level;
    g_dmg_config.brightness_level = persistent_brightness_level;
    g_dmg_config.save_slot        = persistent_save_slot;
    g_dmg_config.load_slot        = persistent_load_slot;
    /* dmg_config_t has no fps field, so it rides in the spare extra_opt1. */
    g_dmg_config.extra_opt1       = persistent_display_fps ? 1u : 0u;
    /* extra_opt2 holds filter mode: 0 = Shimmerless (default), 1 = Sharp */
    g_dmg_config.extra_opt2       = persistent_shimmerless_enabled ? 0u : 1u;
    dmg_config_save(&s_sms_desc);
}

static void sms_load_config(void) {
    /* dmg_config_load() pushes scale/effect/interleave/speed/volume/brightness
     * back into the persistent_* variables through the descriptor callbacks.
     * extra_opt1 has no callback of its own, so the FPS overlay is read and
     * applied by hand. */
    dmg_config_load(&s_sms_desc);
    persistent_display_fps = (g_dmg_config.extra_opt1 != 0u);
    if (s_sms_desc.apply_fps != NULL) s_sms_desc.apply_fps(persistent_display_fps);
    persistent_shimmerless_enabled = (g_dmg_config.extra_opt2 == 0u);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}


static bool sms_get_fps(void) {
    return persistent_display_fps;
}

static void sms_apply_fps(bool on) {
    persistent_display_fps = on;
    ili9341_set_fps_overlay(persistent_display_fps, fpsString);
}

static void rom_file_selector(char *out_selectedRom) {
    dmg_rom_selector(&s_sms_desc, out_selectedRom, FF_MAX_LFN);
}

static dmg_action_t ingame_menu(const char *game_name) {
    return dmg_ingame_menu(&s_sms_desc, game_name);
}

/* True when ROM_FILE_ADDR points at a PSRAM buffer this file allocated, so the
 * battery-backed save logic does not try to carve its SRAM out of the flash
 * window. The PSRAM window is at 0x11000000, which is neither flash XIP
 * (>= 0x10000000) nor SRAM (0x20000000), so address-based checks would not
 * recognise it -- hence a flag rather than a range test. */
static bool s_rom_in_psram = false;

/* Loads the ROM into PSRAM instead of programming it into flash.
 *
 * Mirrors nes_load_rom_to_psram() in the PicoNES driver, for the same reason:
 * the flash path below derives the target as ROM_FILE_ADDR - XIP_BASE, and
 * initAll() only assigns ROM_FILE_ADDR on the flash branch. With a PSRAM present
 * it stays 0, the subtraction wraps to 0xF0000000, and flash_range_erase()
 * rejects that -- the load dies on the first chunk with the bar at zero.
 *
 * Reads in chunks so the progress bar tracks real progress and a short read
 * stops the loop instead of leaving the buffer tail uninitialised. */
static bool sms_load_rom_to_psram(const struct dmg_emulator_desc *desc,
                                  FIL *fil, uint32_t rom_file_size,
                                  const char *display_name) {
    if (rom_file_size == 0) {
        printf("[E] Refusing to load a zero-byte ROM into PSRAM\n");
        return false;
    }

    // f_malloc() panics rather than returning NULL, so the size has to be
    // checked up front. The margin covers the emulator's own allocations
    // (work RAM, VRAM banks) plus lwmem bookkeeping.
    const uint32_t margin = 512u * 1024u;
    const uint32_t avail = (uint32_t)Frens::GetAvailableMemory();
    if (avail > margin && rom_file_size > (avail - margin)) {
        printf("[E] ROM needs %u bytes, only %u usable in PSRAM\n",
               (unsigned)rom_file_size, (unsigned)(avail - margin));
        return false;
    }

    // Free the previous cart before allocating, or switching games leaks each
    // ROM in full and eventually starves the allocator.
    if (s_rom_in_psram && ROM_FILE_ADDR != 0) {
        printf("[I] Freeing previous PSRAM ROM at 0x%08x\n", (unsigned)ROM_FILE_ADDR);
        Frens::f_free((void *)ROM_FILE_ADDR);
        ROM_FILE_ADDR = 0;
        s_rom_in_psram = false;
    }

    uint8_t *buf = (uint8_t *)Frens::f_malloc(rom_file_size);
    if (!buf) {
        printf("[E] PSRAM allocation of %u bytes failed\n", (unsigned)rom_file_size);
        return false;
    }

    // Published before the read, so a failed load is still recognisable and the
    // save logic does not compute a flash address from a stale value.
    ROM_FILE_ADDR = (uintptr_t)buf;
    s_rom_in_psram = true;

    const uint32_t PSRAM_READ_CHUNK = 64u * 1024u;
    uint32_t bytes_loaded = 0;
    while (bytes_loaded < rom_file_size) {
        uint32_t want = rom_file_size - bytes_loaded;
        if (want > PSRAM_READ_CHUNK) want = PSRAM_READ_CHUNK;
        UINT br = 0;
        if (f_read(fil, buf + bytes_loaded, want, &br) != FR_OK || br != want) {
            printf("[E] f_read failed at %u\n", (unsigned)bytes_loaded);
            Frens::f_free(buf);
            ROM_FILE_ADDR = 0;
            s_rom_in_psram = false;
            return false;
        }
        bytes_loaded += br;
        dmg_update_loading_progress(desc, bytes_loaded, rom_file_size);
    }

    printf("[I] ROM loaded to PSRAM: %u bytes at 0x%08x\n",
           (unsigned)rom_file_size, (unsigned)ROM_FILE_ADDR);
    return true;
}

static bool flashrom(const char *path, int *out_fileSize) {
    char full_path[256];
    const char *curr_dir = dmg_get_current_rom_dir();
    if (strchr(path, '/') != NULL || strchr(path, '\\') != NULL || curr_dir == NULL || curr_dir[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "%s", path);
    } else {
        size_t dlen = strlen(curr_dir);
        if (dlen > 0 && curr_dir[dlen - 1] == '/') {
            snprintf(full_path, sizeof(full_path), "%s%s", curr_dir, path);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", curr_dir, path);
        }
    }

    ili9341_blitter_stop();

    FIL fil;
    FRESULT fr = f_open(&fil, full_path, FA_READ);
    if (fr != FR_OK) {
        printf("[E] f_open(%s) error: %d\n", full_path, fr);
        return false;
    }

    uint32_t rom_file_size = (uint32_t)f_size(&fil);

    /* A PSRAM board never programs the ROM into flash: flash_range_erase() and
     * flash_range_program() are slow and destructive, and on this console the
     * flash window is also where the loader image lives. Route to PSRAM before
     * deriving flash_offset, which is meaningless without an XIP base. */
    if (Frens::isPsramEnabled()) {
        const char *pname = strrchr(full_path, '/');
        pname = pname ? (pname + 1) : full_path;
        // Draw the card before reading, or the progress updates land on
        // whatever was on screen before and the bar never appears to move.
        dmg_draw_loading_screen(&s_sms_desc, "CARREGANDO ROM", pname, rom_file_size);
        bool ok = sms_load_rom_to_psram(&s_sms_desc, &fil, rom_file_size, pname);
        f_close(&fil);
        if (ok && !dmg_boot_intro_active()) {
            ili9341_blitter_start();
            ili9341_fill_screen(0x0000);
            ili9341_flush(true);
        }
        if (out_fileSize) *out_fileSize = ok ? (int)rom_file_size : 0;
        return ok;
    }

    const uint8_t *flash_rom = (const uint8_t *)ROM_FILE_ADDR;
    uint32_t flash_offset = (uint32_t)(ROM_FILE_ADDR - XIP_BASE);

    /* Is this ROM already in flash? With the size known from this session a
     * first-sector check is enough. After a reboot the size is unknown (0) --
     * the normal case on a standby resume -- so the whole file is compared,
     * which stops at the first differing sector for another ROM and saves a
     * full erase/program for the same one. */
    bool already_flashed = false;
    if (rom_file_size > 0 &&
        (last_flashed_rom_size == rom_file_size || last_flashed_rom_size == 0)) {
        const bool full_cmp = (last_flashed_rom_size == 0);
        static uint8_t chk[FLASH_SECTOR_SIZE];
        uint32_t cmp_off = 0;
        already_flashed = true;
        while (cmp_off < rom_file_size) {
            UINT cbr = 0;
            fr = f_read(&fil, chk, sizeof(chk), &cbr);
            if (fr != FR_OK || cbr == 0 || memcmp(chk, flash_rom + cmp_off, cbr) != 0) {
                already_flashed = false;
                break;
            }
            cmp_off += cbr;
            if (!full_cmp) break;
            watchdog_update();
        }
        f_lseek(&fil, 0);
        if (already_flashed) last_flashed_rom_size = rom_file_size;
    }

    if (already_flashed) {
        printf("[I] ROM already in flash, skipping write\n");
    } else {
        printf("[I] Programming ROM into flash (%u bytes)...\n", (unsigned)rom_file_size);
        const char *name = strrchr(full_path, '/'); name = name ? (name + 1) : full_path;
        dmg_draw_loading_screen(&s_sms_desc, "CARREGANDO ROM", name, rom_file_size);
        Frens::blinkLed(true);
        dmg_boot_intro_flash_begin();
        static uint8_t block_buf[FLASH_BLOCK_SIZE];
        uint32_t bytes_loaded = 0;
        uint32_t cur_flash_offset = flash_offset;
        UINT br;

        while (bytes_loaded < rom_file_size) {
            memset(block_buf, 0xFF, FLASH_BLOCK_SIZE);
            fr = f_read(&fil, block_buf, FLASH_BLOCK_SIZE, &br);
            if (fr != FR_OK || br == 0) break;

            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(cur_flash_offset, FLASH_BLOCK_SIZE);
            restore_interrupts(ints);

            const uint32_t prog_len = (br + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
            for (uint32_t off = 0; off < prog_len; off += FLASH_SECTOR_SIZE) {
                uint32_t chunk = (prog_len - off > FLASH_SECTOR_SIZE) ? FLASH_SECTOR_SIZE : (prog_len - off);
                chunk = (chunk + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
                ints = save_and_disable_interrupts();
                flash_range_program(cur_flash_offset + off, block_buf + off, chunk);
                restore_interrupts(ints);
            }

            bytes_loaded += br;
            cur_flash_offset += FLASH_BLOCK_SIZE;

            if (rom_file_size > 0) {
                dmg_update_loading_progress(&s_sms_desc, bytes_loaded, rom_file_size);
            }
        }
        dmg_boot_intro_flash_end();
        last_flashed_rom_size = rom_file_size; Frens::blinkLed(false);
    }
    f_close(&fil);

    *out_fileSize = (int)rom_file_size;
    uint32_t final_crc = update_crc32(0, flash_rom, rom_file_size);
    Frens::setCrcOfLoadedRom(final_crc);
    printf("[I] ROM ready in flash: %u bytes, CRC: 0x%08X\n", (unsigned)rom_file_size, (unsigned)final_crc);
    return true;
}

/* =========================================================================
 * Gameplay Input Reading & Combos
 * ========================================================================= */
static void process_game_inputs(void) {
    dmg_pad_t pad_in;
    read_ui_buttons(&pad_in.up, &pad_in.down, &pad_in.left, &pad_in.right,
                    &pad_in.a, &pad_in.b, &pad_in.select, &pad_in.start);

    dmg_combo_events_t ev;
    dmg_combo_frame(&pad_in, &s_sms_desc, &ev);

    if (ev.request_pause_menu) s_request_pause_menu = true;
    if (ev.request_exit_game)  s_request_exit_game  = true;
    if (ev.request_power_off) {
        s_request_power_off = true;
        s_power_reason = ev.power_reason;
    }
    if (ev.fps_toggled) {
        printf("[I] Combo START+A: FPS display = %s\n",
               persistent_display_fps ? "ON" : "OFF");
    }
    if (ev.speed_toggled) {
        printf("[I] Combo SELECT+A: speed = %s\n",
               persistent_turbo_mode ? "2x" : "1x");
    }

    bool up = pad_in.up, down = pad_in.down;
    bool left = pad_in.left, right = pad_in.right;
    bool a = pad_in.a, b = pad_in.b;
    bool select = pad_in.select, start = pad_in.start;

    uint32_t pad = 0;
    if (up)    pad |= INPUT_UP;
    if (down)  pad |= INPUT_DOWN;
    if (left)  pad |= INPUT_LEFT;
    if (right) pad |= INPUT_RIGHT;
    if (a)     pad |= INPUT_BUTTON2; /* A -> Button 2 */
    if (b)     pad |= INPUT_BUTTON1; /* B -> Button 1 */

    input.pad[0] = pad;
    input.pad[1] = 0;

    uint32_t sys = 0;
#if SMS_IS_GG_BUILD
    if (start) sys |= INPUT_START;
#else
    if (start || select) sys |= INPUT_PAUSE;
#endif
    input.system = sys;
}

/* =========================================================================
 * Emulation Frame Loop (Process)
 * ========================================================================= */
static void process(void) {
    s_request_pause_menu = false;
    s_request_exit_game = false;
    reset = false;
    /* Drop any half-finished SELECT+START hold or closed edge latch left over
     * from the previous game, so the first hold of this one starts from zero. */
    dmg_combo_reset();

    uint32_t frame_count = 0;
    uint32_t last_fps_us = time_us_32();
    uint32_t deadline = time_us_32();

    while (!reset && !s_request_exit_game) {
        process_game_inputs();

        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (s_request_power_off) {
            s_request_power_off = false;
            ili9341_blitter_stop();
            dmg_power_off_with_resume(&s_sms_desc, (dmg_power_reason_t)s_power_reason);
        }

        if (s_request_pause_menu) {
            s_request_pause_menu = false;
            ili9341_blitter_stop();
            ingame_action_e act = ingame_menu(selectedRom);
            if (act == INGAME_ACTION_RESET) {
                system_reset();
            } else if (act == INGAME_ACTION_EXIT) {
                system_save_sram();
                s_request_exit_game = true;
                break;
            }
            ili9341_clear_black();
            ili9341_blitter_start();
            deadline = time_us_32();
        }

        if (s_request_exit_game) {
            break;
        }

        /* Run SmsPlus frame */
        sms_frame(0);
        if (persistent_turbo_mode) {
            sms_frame(1);
        }

        /* Pump audio batch to I2S DAC */
        processaudioPerFrameI2S();

        /* Invert field parity for interlacing */
        ili9341_stream_sms_frame_end();

        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            ili9341_blitter_stop();
            const bool resumed = sms_load_state(selectedRom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_clear_black();
            ili9341_blitter_start();
            deadline = time_us_32();
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }

        /* FPS calculation */
        frame_count++;
        uint32_t now_us = time_us_32();
        if (now_us - last_fps_us >= 1000000) {
            fps = frame_count;
            frame_count = 0;
            last_fps_us = now_us;
            snprintf(fpsString, sizeof(fpsString), "%02u", (unsigned)(fps > 99 ? 99 : fps));
        }

        ili9341_set_fps_overlay(persistent_display_fps, fpsString);

        /* 60 FPS pacing (16667 us / frame) */
        deadline += 16667u;
        const uint32_t now = time_us_32();
        if (!persistent_turbo_mode && (int32_t)(deadline - now) > 0) {
            busy_wait_us_32((uint32_t)(deadline - now));
        } else {
            deadline = now;
        }
    }
}

/* =========================================================================
 * Main Program Entry Point
 * ========================================================================= */
int main(void) {
    vreg_voltage voltage = VREG_VOLTAGE_1_30;
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));

    printf("==========================================================================================\n");
    printf("%s for RP2350 + ILI9341\n", APP_TITLE);
    printf("Build date: %s %s\n", __DATE__, __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
    printf("==========================================================================================\n");

    init_ui_buttons();
    selectedRom[0] = '\0';
    ErrorMessage[0] = '\0';

    FrensSettings::initSettings(FrensSettings::emulators::SMS);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, AUDIOBUFFERSIZE, false, true);

#if USE_ILI9341
    dmg_init_boot_mode();
    ili9341_init();
    dmg_config_load(&s_sms_desc);
    set_backlight_brightness(persistent_brightness_level);
    set_sound_volume_level(persistent_volume_level);
    ili9341_set_scale_mode(persistent_scale_mode);
#if SMS_IS_GG_BUILD
    ili9341_set_grid(persistent_effect_enabled);
    ili9341_set_is_gamegear(true);
#else
    ili9341_set_scanlines(persistent_effect_enabled);
    ili9341_set_is_gamegear(false);
#endif
    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_sms_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_sms_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_sms_desc);
            snprintf(selectedRom, FF_MAX_LFN, "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_sms_desc)) dmg_boot_intro_finish();
        }
    }
#endif

    while (true) {
        if (strlen(selectedRom) == 0) {
            rom_file_selector(selectedRom);
            printf("[I] Playing selected ROM: %s\n", selectedRom);
        }

        int fileSize = 0;
        if (!flashrom(selectedRom, &fileSize)) {
            printf("[E] Failed to load ROM into flash\n");
            if (s_resume_pending) {
                s_resume_pending = false;
                dmg_resume_consume();
            }
            dmg_boot_intro_finish();
            selectedRom[0] = '\0';
            continue;
        }

        /* The config file is the source of truth for how the game is displayed,
         * so it is re-read when a game is entered, not only at boot. Safe to
         * repeat because every menu visit commits first, so the file always
         * matches what was on screen when the menu closed. */
        sms_load_config();
        set_backlight_brightness(persistent_brightness_level);
        set_sound_volume_level(persistent_volume_level);
        ili9341_set_scale_mode(persistent_scale_mode);
#if SMS_IS_GG_BUILD
        ili9341_set_grid(persistent_effect_enabled);
#else
        ili9341_set_scanlines(persistent_effect_enabled);
#endif
        ili9341_set_interleave(persistent_interleave);

        /* Detect type */
        const char *dot = strrchr(selectedRom, '.');
        if (dot && strcasecmp(dot, ".gg") == 0) {
            isGameGear = true;
        } else {
#if SMS_IS_GG_BUILD
            isGameGear = true;
#else
            isGameGear = false;
#endif
        }

        ili9341_set_is_gamegear(isGameGear);
        cart.type = isGameGear ? TYPE_GG : TYPE_SMS;

        /* Load ROM into SmsPlus core */
        load_rom(ROM_FILE_ADDR, fileSize, isGameGear);
        system_init(SMS_AUD_RATE);
#if PICO_RP2350
        sms.use_fm = 1;
#endif
        system_reset();
        system_load_sram();

        dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */

        /* Insert a black frame to eliminate menu remnants */
        ili9341_clear_black();

        /* Start display blitter worker on Core 1 */
        ili9341_blitter_start();

        /* Run emulation */
        dmg_power_set_game(selectedRom, selectedRom);
        process();
        dmg_power_set_game(NULL, NULL);

        /* Stop blitter and tear down emulator state */
        ili9341_blitter_stop();
        system_save_sram();
        system_shutdown();

        selectedRom[0] = '\0';
    }

    return 0;
}
