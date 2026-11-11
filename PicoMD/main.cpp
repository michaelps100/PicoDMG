/**
 * PicoMD for Raspberry Pi RP2350 + ILI9341 SPI TFT + MAX98357A I2S
 *
 * Sega Mega Drive / Genesis Emulator with Clean Light UI,
 * In-Game Pause Menu, Save/Load States, Scanlines Effect,
 * 60 FPS Interpolated Display Pipeline, and SD Card Storage.
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
#include "hardware/pll.h"

#include "ff.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "external_audio.h"
#include "state.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"

#if USE_ILI9341
#include "ili9341.h"
#endif

/* Gwenesis emulator core headers */
extern "C" {
#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "gwenesis_io.h"
#include "gwenesis_savestate.h"
#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwsnd.h"
#include "buffers.h"
#include "gwsram.h"

extern unsigned char gwenesis_vdp_regs[];
extern unsigned short gwenesis_vdp_status;
extern int screen_width;
extern int screen_height;
extern int hint_pending;
extern int scan_line;
extern int frame_counter;
extern int system_clock;
extern unsigned char button_state[3];
}

/* =========================================================================
 * Build Target Configuration (PicoMD)
 * ========================================================================= */
#define APP_TITLE            "PicoMD"
#define CONFIG_MAGIC         0x4D445350u /* "PSMD" */
#define CONFIG_PATH          "0:/saves/md/picomd.cfg"
#define CONFIG_PATH_FALLBACK "/saves/md/picomd.cfg"
#define SAVES_DIR            "0:/saves/md"
#define SAVES_DIR_FALLBACK   "/saves/md"
#define ROMS_DIR             "0:/roms/md"
#define ROMS_DIR_FALLBACK    "/roms/md"

#define MD_ROM_CACHE_HEADROOM 0x100000u /* 1 MB offset, 64KB aligned, safe beyond ~750KB code */
#define MD_ROM_CACHE_MAX      0x800000u /* 8 MB ROM cache */
#define MD_ROM_CACHE_ABS      (0x10080000u + MD_ROM_CACHE_HEADROOM)
#define MD_ROM_CACHE_OFF      (MD_ROM_CACHE_ABS - 0x10000000u)

#if PICO_RP2350
#define EMULATOR_CLOCKFREQ_KHZ 378000
#include "hardware/structs/qmi.h"
#else
#define EMULATOR_CLOCKFREQ_KHZ 252000
#endif
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

#if PICO_RP2350
static void __no_inline_not_in_flash_func(apply_safe_qmi_timing)(void) {
    const uint32_t max_flash_khz = 94500;
    uint32_t flash_div = (CPUFreqKHz + max_flash_khz - 1) / max_flash_khz;
    if (flash_div < 4)
        flash_div = 4;
    uint32_t rxdelay = (3 * CPUFreqKHz + 189000) / 378000;
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

static bool isFatalError = false;
static char selectedRom[FF_MAX_LFN];
static uint32_t last_flashed_rom_size = 0;

static uint32_t fps = 0;
static char fpsString[8] = "60";

static uint32_t s_audio_batch[1024];
static uint64_t s_audio_sample_accum = 0;

static uint16_t s_scanline_buffer[320];

/* =========================================================================
 * Persistent Configuration Structure
 * ========================================================================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t scale_mode;        /* 0 = Fit, 1 = Stretch, 2 = Integer */
    uint32_t effect_enabled;    /* 0 = None, 1 = Scanlines */
    uint32_t display_fps;       /* 0 = Off, 1 = On */
    uint32_t volume_level;      /* 1..5, default 3 */
    uint32_t brightness_level;  /* 1..5, default 5 */
    uint32_t turbo_mode;        /* 0 = Normal 1x, 1 = Turbo 2x */
    uint32_t save_slot;         /* 1..4, default 1 */
    uint32_t load_slot;
    uint32_t interleave;         /* 1..4, default 1 */
} emulator_config_t;

static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_scanlines_enabled = false;
static bool persistent_shimmerless_enabled = true;
static bool persistent_interleave = true;
static bool persistent_display_fps = false;
static uint8_t persistent_volume_level = 3;     /* 1..5 */
static uint8_t persistent_brightness_level = 5; /* 1..5 */
static uint8_t persistent_turbo_mode = 0;       /* 0 = 1x, 1 = 2x */
static uint8_t persistent_save_slot = 1;        /* 1..4 */
static uint8_t persistent_load_slot = 1;        /* 1..4 */

enum md_frameskip_mode_t {
    FRAMESKIP_OFF  = 0,
    FRAMESKIP_AUTO = 1,
    FRAMESKIP_ON   = 2
};
static uint8_t persistent_frameskip_mode = FRAMESKIP_AUTO;
static bool persistent_force_h32 = false;
static bool persistent_fast_audio = false;

static bool s_request_pause_menu = false;
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
    f_mkdir("/saves");
    f_mkdir(SAVES_DIR_FALLBACK);
}

/* Static buffers to prevent stack overflow */
static char current_rom_dir[64] = ROMS_DIR;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;
static char s_full_path[FF_MAX_LFN + 32];
static FIL  s_io_fil;

#define get_sanitized_basename dmg_sanitize_basename
#define build_state_path(g, s, out, len) dmg_build_state_path(SAVES_DIR, g, s, out, len)

extern const dmg_emulator_desc_t s_md_desc;
#define save_emulator_config() md_save_config()
#define load_emulator_config() md_load_config()

static void system_load_sram(const char *game_name) {
    if (!gwsram_battery || gwsram_bytes == 0) return;
    if (gwsram_ensure_buffer() != 0) return;

    char base[64];
    get_sanitized_basename(game_name, base, sizeof(base));

    char sram_path[FF_MAX_LFN];
    snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR, base);
    FRESULT fr = f_open(&s_io_fil, sram_path, FA_READ);
    if (fr != FR_OK) {
        snprintf(sram_path, sizeof(sram_path), "%s/%s.sav", SAVES_DIR, base);
        fr = f_open(&s_io_fil, sram_path, FA_READ);
    }
    if (fr != FR_OK) {
        snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR_FALLBACK, base);
        fr = f_open(&s_io_fil, sram_path, FA_READ);
    }
    if (fr == FR_OK) {
        UINT br = 0;
        f_read(&s_io_fil, gwsram_data, gwsram_bytes, &br);
        f_close(&s_io_fil);
        gwsram_dirty = 0;
        printf("[I] Battery SRAM loaded (%u bytes) from %s\n", (unsigned)br, sram_path);
    }
}

static void system_save_sram(const char *game_name) {
    if (!gwsram_battery || !gwsram_data || gwsram_bytes == 0 || !gwsram_dirty) return;
    ensure_save_directories();

    char base[64];
    get_sanitized_basename(game_name, base, sizeof(base));

    char sram_path[FF_MAX_LFN];
    snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR, base);
    FRESULT fr = f_open(&s_io_fil, sram_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR_FALLBACK, base);
        fr = f_open(&s_io_fil, sram_path, FA_WRITE | FA_CREATE_ALWAYS);
    }
    if (fr == FR_OK) {
        UINT bw = 0;
        f_write(&s_io_fil, gwsram_data, gwsram_bytes, &bw);
        f_close(&s_io_fil);
        gwsram_dirty = 0;
        printf("[I] Battery SRAM saved (%u bytes) to %s\n", (unsigned)bw, sram_path);
    }
}

/* =========================================================================
 * Audio Processing (gwsnd Resampler -> MAX98357A I2S)
 * ========================================================================= */
static inline int16_t md_sat16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void __not_in_flash_func(audioOutI2S)(int16_t l, int16_t r) {
    (void)r;
    static const int32_t vol_table[6] = { 0, 51, 102, 154, 205, 256 };
    int32_t vol = vol_table[persistent_volume_level];
    if (vol > 0) {
        int32_t s = ((int32_t)l * vol) >> 8;
        int16_t s16 = md_sat16(s);
        EXT_AUDIO_ENQUEUE_SAMPLE(s16, s16);
    } else {
        EXT_AUDIO_ENQUEUE_SAMPLE(0, 0);
    }
}

/* =========================================================================
 * Clean Light UI via shared_menu
 * ========================================================================= */

typedef dmg_action_t ingame_action_e;
#define INGAME_ACTION_RESUME DMG_ACTION_RESUME
#define INGAME_ACTION_RESET  DMG_ACTION_RESET
#define INGAME_ACTION_EXIT   DMG_ACTION_EXIT

static const char *s_md_candidate_dirs[] = {
    "0:/roms/md",
    "/roms/md",
    "roms/md",
    "0:/ROMS/MD",
    "/ROMS/MD",
    "ROMS/MD",
    "0:/roms/genesis",
    "/roms/genesis",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};

static bool is_md_rom(const char *name) {
    if (!name || name[0] == '\0') return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".md") == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".gen") == 0 ||
            strcasecmp(dot, ".smd") == 0);
}

static bool md_save_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    ensure_save_directories();
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));
    return (Emulator_SaveState(path) == 0);
}

static bool md_load_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));

    /* The sound engine keeps running on core 1 (the blitter's background
     * task) even with the blitter stopped, over the same YM2612/SN76489 the
     * load is about to overwrite, with events of the old timeline still in its
     * FIFO. Detach it first, load, start it again on an empty FIFO and copy
     * the restored timers into the core0 status shadow -- otherwise the audio
     * kept going but no longer matched the game (resume, and any load). */
    const bool offload = gwsnd_is_offload_active();
    if (offload) gwsnd_shutdown();
    const bool ok = (Emulator_LoadState(path) == 0);
    if (offload) {
        gwsnd_init((gwenesis_vdp_status & 1) != 0, 1);
        if (!gwsnd_is_offload_active()) gwsnd_set_output(audioOutI2S);
    }
    gwsnd_shadow_sync_from_chip();
    return ok;
}

static void md_apply_scale(dmg_scale_mode_t mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void md_apply_effect(bool effect) {
    persistent_scanlines_enabled = effect;
    ili9341_set_scanlines(persistent_scanlines_enabled);
}

static void md_apply_interleave(bool interleave) {
    persistent_interleave = interleave;
    ili9341_set_interleave(persistent_interleave);
}

static void md_apply_speed(bool turbo) {
    persistent_turbo_mode = turbo ? 1 : 0;
}

static void md_apply_volume(uint8_t vol) {
    set_sound_volume_level(vol);
}

static void md_apply_brightness(uint8_t br) {
    set_backlight_brightness(br);
}

static void md_apply_filter(bool shimmerless) {
    persistent_shimmerless_enabled = shimmerless;
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

/* Combo hooks, defined after the descriptor they are stored in. */
static void md_save_config(void);
static bool md_get_fps(void);
static void md_apply_fps(bool on);

static void draw_md_image_row(uint8_t idx, bool selected, ili9341_scale_mode_t scale, bool scanlines, bool shimmerless, bool interleave, uint8_t frameskip, bool force_h32, bool fast_audio) {
    if (idx >= 7) return;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? DMG_RGB565_SEL_COLOR : DMG_RGB565_BG_COLOR;

    ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    char buf[44];
    buf[0] = '\0';

    if (idx == 0) {
        const char *scale_names[] = { "< Fit >", "< Stretch >", "< Integer >" };
        int sm = (int)scale;
        if (sm < 0 || sm >= 3) sm = 0;
        snprintf(buf, sizeof(buf), "Scale:        %s", scale_names[sm]);
    } else if (idx == 1) {
        snprintf(buf, sizeof(buf), "Effect:       %s", scanlines ? "< Scanlines >" : "< None >");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Filter:       %s", shimmerless ? "< Shimmerless >" : "< Sharp >");
    } else if (idx == 3) {
        snprintf(buf, sizeof(buf), "Screen:       %s", interleave ? "< Interleave >" : "< Progressive >");
    } else if (idx == 4) {
        const char *fs_names[] = { "< Off >", "< Auto >", "< On >" };
        snprintf(buf, sizeof(buf), "Frameskip:    %s", fs_names[frameskip % 3]);
    } else if (idx == 5) {
        snprintf(buf, sizeof(buf), "Res Mode:     %s", force_h32 ? "< Force H32 >" : "< Auto >");
    } else if (idx == 6) {
        snprintf(buf, sizeof(buf), "Audio Rate:   %s", fast_audio ? "< Fast 26k >" : "< Full 53k >");
    }

    ili9341_text(buf, (uint16_t)(wx + 12u), (uint16_t)(y + 7u), selected ? DMG_RGB565_TEXT_SEL_COLOR : DMG_RGB565_TEXT_COLOR, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void md_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0; /* 0: Scale, 1: Effect, 2: Filter, 3: Screen, 4: Frameskip, 5: Res Mode, 6: Audio Rate */
    const uint8_t max_items = 7;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;

    ili9341_scale_mode_t sel_scale = persistent_scale_mode;
    bool sel_scanlines = persistent_scanlines_enabled;
    bool sel_shimmerless = persistent_shimmerless_enabled;
    bool sel_interleave = persistent_interleave;
    uint8_t sel_frameskip = persistent_frameskip_mode;
    bool sel_force_h32 = persistent_force_h32;
    bool sel_fast_audio = persistent_fast_audio;

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

    for (uint8_t i = 0; i < max_items; i++) {
        draw_md_image_row(i, i == cursor, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
    }
    for (uint8_t i = max_items; i < 8; i++) {
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
            cursor = (cursor == 0) ? (max_items - 1) : (cursor - 1);
            draw_md_image_row(prev, false, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            draw_md_image_row(cursor, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == max_items - 1) ? 0 : (cursor + 1);
            draw_md_image_row(prev, false, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            draw_md_image_row(cursor, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
        }

        if (pushed_left || pushed_right || pushed_a) {
            if (cursor == 0) {
                int num_modes = 3;
                if (pushed_right || pushed_a) {
                    sel_scale = (ili9341_scale_mode_t)(((int)sel_scale + 1) % num_modes);
                } else {
                    sel_scale = (ili9341_scale_mode_t)(((int)sel_scale == 0) ? (num_modes - 1) : ((int)sel_scale - 1));
                }
                draw_md_image_row(0, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            } else if (cursor == 1) {
                sel_scanlines = !sel_scanlines;
                draw_md_image_row(1, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            } else if (cursor == 2) {
                sel_shimmerless = !sel_shimmerless;
                draw_md_image_row(2, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            } else if (cursor == 3) {
                sel_interleave = !sel_interleave;
                draw_md_image_row(3, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            } else if (cursor == 4) {
                if (pushed_right || pushed_a) {
                    sel_frameskip = (sel_frameskip + 1) % 3;
                } else {
                    sel_frameskip = (sel_frameskip == 0) ? 2 : (sel_frameskip - 1);
                }
                draw_md_image_row(4, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            } else if (cursor == 5) {
                sel_force_h32 = !sel_force_h32;
                draw_md_image_row(5, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            } else if (cursor == 6) {
                sel_fast_audio = !sel_fast_audio;
                draw_md_image_row(6, true, sel_scale, sel_scanlines, sel_shimmerless, sel_interleave, sel_frameskip, sel_force_h32, sel_fast_audio);
            }
        }

        if (pushed_b) {
            persistent_scale_mode = sel_scale;
            persistent_scanlines_enabled = sel_scanlines;
            persistent_shimmerless_enabled = sel_shimmerless;
            persistent_interleave = sel_interleave;
            persistent_frameskip_mode = sel_frameskip;
            persistent_force_h32 = sel_force_h32;
            persistent_fast_audio = sel_fast_audio;
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_set_interleave(persistent_interleave);
            gwsnd_set_audio_rate(persistent_fast_audio ? 1 : 0);
            md_save_config();
            dmg_wait_for_all_buttons_released();
            return;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}

#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
/* Resume intro chime, pushed from core 1 into the I2S ring (dmg_boot_intro.c). */
static int  md_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void md_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void md_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(44100);
}
#endif

/* Battery SRAM to the SD before a standby (dmg_power_off_with_resume). */
static void md_flush_saves(void) { system_save_sram(selectedRom); }

const dmg_emulator_desc_t s_md_desc = {
    .app_title            = APP_TITLE,
    .default_rom_dir      = ROMS_DIR,
    .candidate_dirs       = s_md_candidate_dirs,
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
    .is_rom_file          = is_md_rom,
    .on_load_rom          = NULL,
    .save_state           = md_save_state,
    .load_state           = md_load_state,
    .apply_scale          = md_apply_scale,
    .apply_effect         = md_apply_effect,
    .apply_interleave     = md_apply_interleave,
    .apply_speed          = md_apply_speed,
    .apply_volume         = md_apply_volume,
    .apply_brightness     = md_apply_brightness,
    .apply_filter         = md_apply_filter,
    .reset_game           = NULL,
    .exit_game            = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu       = md_image_submenu,
    /* On the console build (PSRAM_CS_PIN != 0) the ROM is read straight from the
     * SD card into PSRAM, so the loading screen must not claim to write a flash
     * cache it never touches. The HW15 build has no PSRAM and still programs
     * flash, so there the default wording is correct -- hence the pin test. */
    .loading_status       = (PSRAM_CS_PIN != 0) ? "Writing PSRAM cache..." : NULL,
    .save_config          = md_save_config,
    .get_fps              = md_get_fps,
    .apply_fps            = md_apply_fps,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_free           = md_audio_free,
    .audio_push           = md_audio_push,
#endif
    .flush_saves          = md_flush_saves,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_restore        = md_audio_restore
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
static void md_save_config(void) {
    g_dmg_config.scale_mode       = (uint32_t)persistent_scale_mode;
    g_dmg_config.grid_effect      = persistent_scanlines_enabled ? 1u : 0u;
    g_dmg_config.interleave       = persistent_interleave ? 1u : 0u;
    g_dmg_config.turbo_mode       = persistent_turbo_mode;
    g_dmg_config.volume_level     = persistent_volume_level;
    g_dmg_config.brightness_level = persistent_brightness_level;
    g_dmg_config.save_slot        = persistent_save_slot;
    g_dmg_config.load_slot        = persistent_load_slot;
    /* dmg_config_t has no fps field, so it rides in the spare extra_opt1. */
    g_dmg_config.extra_opt1       = persistent_display_fps ? 1u : 0u;
    /* extra_opt2 holds:
     * bit 0: filter mode (0 = Shimmerless, 1 = Sharp)
     * bits 1..2: frameskip (0: Off, 1: Auto, 2: On)
     * bit 3: force H32 mode (0: Auto, 1: Force H32)
     * bit 4: fast audio mode (0: Full 53k, 1: Fast 26k) */
    g_dmg_config.extra_opt2       = (persistent_shimmerless_enabled ? 0u : 1u) |
                                    ((uint32_t)(persistent_frameskip_mode & 3u) << 1) |
                                    (persistent_force_h32 ? (1u << 3) : 0u) |
                                    (persistent_fast_audio ? (1u << 4) : 0u);
    dmg_config_save(&s_md_desc);
}

static void md_load_config(void) {
    /* dmg_config_load() pushes scale/effect/interleave/speed/volume/brightness
     * back into the persistent_* variables through the descriptor callbacks.
     * extra_opt1 has no callback of its own, so the FPS overlay is read and
     * applied by hand. */
    dmg_config_load(&s_md_desc);
    persistent_display_fps = (g_dmg_config.extra_opt1 != 0u);
    if (s_md_desc.apply_fps != NULL) s_md_desc.apply_fps(persistent_display_fps);
    persistent_shimmerless_enabled = ((g_dmg_config.extra_opt2 & 1u) == 0u);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    persistent_frameskip_mode = (uint8_t)((g_dmg_config.extra_opt2 >> 1) & 3u);
    if (persistent_frameskip_mode > 2) persistent_frameskip_mode = FRAMESKIP_AUTO;
    persistent_force_h32 = ((g_dmg_config.extra_opt2 & (1u << 3)) != 0u);
    persistent_fast_audio = ((g_dmg_config.extra_opt2 & (1u << 4)) != 0u);
    gwsnd_set_audio_rate(persistent_fast_audio ? 1 : 0);
}


static bool md_get_fps(void) {
    return persistent_display_fps;
}

static void md_apply_fps(bool on) {
    persistent_display_fps = on;
    ili9341_set_fps_overlay(persistent_display_fps, fpsString);
}

static bool rom_file_selector(char *selected_out, size_t max_len) {
    return dmg_rom_selector(&s_md_desc, selected_out, max_len);
}

static ingame_action_e show_ingame_menu(const char *game_name) {
    return dmg_ingame_menu(&s_md_desc, game_name);
}

/* =========================================================================
 * Flash Caching & ROM Loading
 * ========================================================================= */
static void md_byteswap_words(uint8_t *data, uint32_t size) {
    for (uint32_t i = 0; i + 1u < size; i += 2u) {
        uint8_t t = data[i];
        data[i] = data[i + 1u];
        data[i + 1u] = t;
    }
}

static bool md_is_valid_rom(const uint8_t *rom, uint32_t size) {
    if (!rom || size < 0x200u || (size & 1u) != 0u) return false;
    if (rom[0x100u ^ 1u] == 'S' && rom[0x101u ^ 1u] == 'E' &&
        rom[0x102u ^ 1u] == 'G' && rom[0x103u ^ 1u] == 'A')
        return true;
    const uint32_t pc = ((uint32_t)rom[0x04u ^ 1u] << 24) |
                        ((uint32_t)rom[0x05u ^ 1u] << 16) |
                        ((uint32_t)rom[0x06u ^ 1u] << 8) |
                        (uint32_t)rom[0x07u ^ 1u];
    return ((pc & 1u) == 0u) && pc >= 0x200u && pc < size;
}



/* The cartridge currently loaded, and where it lives.
 *
 * On the HW15 build the ROM is programmed into flash and read back through the
 * XIP window at MD_ROM_CACHE_ABS. On the console build (PSRAM present) it is
 * read from the SD card straight into PSRAM and s_rom_ptr points there
 * instead. A flag rather than an address range test: the PSRAM window is at
 * 0x11000000, which is neither flash XIP (>= 0x10000000) nor SRAM
 * (0x20000000), so a range check would not recognise it. */
static const uint8_t *s_rom_ptr = (const uint8_t *)MD_ROM_CACHE_ABS;
static bool s_rom_in_psram = false;

/* Reads the cartridge into PSRAM instead of programming it into flash.
 *
 * The flash path below writes through safe_flash_range_erase/program, which is
 * slow and destructive, and on this console the flash window also holds the
 * loader image. Mega Drive ROMs reach 4 MB, so on a PSRAM board there is no
 * reason to spend flash or the multi-second erase time.
 *
 * The 512-byte copier header is stripped before this is called, so the buffer
 * holds only the cartridge image, and the words are byte-swapped in place --
 * both matching what the flash path does, so the emulator sees the same bytes
 * either way. Reads in chunks so the progress bar tracks real progress and a
 * short read stops the loop instead of leaving the buffer tail uninitialised. */
static bool md_load_rom_to_psram(FIL *fil, uint32_t size, const char *name) {
    if (size == 0) {
        printf("[E] Refusing to load a zero-byte ROM into PSRAM\n");
        return false;
    }

    // f_malloc() panics (infinite loop) rather than returning NULL, so the fit
    // has to be checked here or the console hangs with a black screen instead
    // of reporting an error. The margin covers the emulator's own allocations
    // (M68K RAM, Z80 RAM, VRAM, YM2612/PSG tables and buffers, cart SRAM) plus
    // lwmem bookkeeping.
    //
    // The comparison must be written as "does not fit" and not guarded by
    // `avail > margin`: with a large cart (Aladdin is ~5 MB) `avail` can land
    // below the margin, the guard goes false, and the allocation is attempted
    // anyway -- straight into panic().
    const uint32_t margin = 1024u * 1024u;
    const uint32_t avail = (uint32_t)Frens::GetAvailableMemory();
    if (size > avail || (avail - size) < margin) {
        printf("[E] ROM needs %u bytes; only %u usable in PSRAM and %u is "
               "reserved for the emulator\n",
               (unsigned)size, (unsigned)avail, (unsigned)margin);
        return false;
    }

    // Free the previous cart before allocating, or switching games leaks each
    // ROM in full and eventually starves the allocator.
    if (s_rom_in_psram && s_rom_ptr) {
        printf("[I] Freeing previous PSRAM ROM at 0x%08x\n", (unsigned)(uintptr_t)s_rom_ptr);
        Frens::f_free((void *)s_rom_ptr);
        s_rom_ptr = NULL;
        s_rom_in_psram = false;
    }

    uint8_t *buf = (uint8_t *)Frens::f_malloc(size);
    if (!buf) {
        printf("[E] PSRAM allocation of %u bytes failed\n", (unsigned)size);
        return false;
    }

    const uint32_t CHUNK = 64u * 1024u;
    uint32_t loaded = 0;
    while (loaded < size) {
        watchdog_update();
        uint32_t want = size - loaded;
        if (want > CHUNK) want = CHUNK;
        UINT got = 0;
        if (f_read(fil, buf + loaded, want, &got) != FR_OK || got != want) {
            printf("[E] SD read error at offset %u\n", (unsigned)loaded);
            Frens::f_free(buf);
            s_rom_ptr = NULL;
            s_rom_in_psram = false;
            return false;
        }
        loaded += got;
        dmg_update_loading_progress(&s_md_desc, loaded, size);
    }

    // Same in-place byte swap the flash path applies, so the cartridge reads
    // identically no matter which memory it came from.
    md_byteswap_words(buf, size);

    s_rom_ptr = buf;
    s_rom_in_psram = true;
    last_flashed_rom_size = size;
    printf("[I] ROM loaded to PSRAM: %s, %u bytes at 0x%08x\n",
           name, (unsigned)size, (unsigned)(uintptr_t)buf);
    return true;
}

static bool load_cart_rom_file(const char *filename) {
    apply_safe_qmi_timing();
    const char *curr_dir = dmg_get_current_rom_dir();
    if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL || curr_dir == NULL || curr_dir[0] == '\0') {
        snprintf(s_full_path, sizeof(s_full_path), "%s", filename);
    } else {
        size_t dlen = strlen(curr_dir);
        if (dlen > 0 && curr_dir[dlen - 1] == '/') {
            snprintf(s_full_path, sizeof(s_full_path), "%s%s", curr_dir, filename);
        } else {
            snprintf(s_full_path, sizeof(s_full_path), "%s/%s", curr_dir, filename);
        }
    }
    FRESULT fr = f_open(&s_io_fil, s_full_path, FA_READ);
    if (fr != FR_OK) {
        snprintf(s_full_path, sizeof(s_full_path), "%s/%s", ROMS_DIR_FALLBACK, filename);
        fr = f_open(&s_io_fil, s_full_path, FA_READ);
    }
    if (fr != FR_OK) {
        printf("[E] Cannot open ROM file: %s\n", filename);
        return false;
    }

    FSIZE_t fsz = f_size(&s_io_fil);
    uint32_t header_offset = 0;
    if ((fsz % 1024) == 512) {
        header_offset = 512;
        fsz -= 512;
        printf("[I] Detected 512-byte copier header - skipping it\n");
    }

    if (fsz < 0x200 || fsz > MD_ROM_CACHE_MAX || (fsz & 1) != 0) {
        printf("[E] Invalid ROM size: %lu\n", (unsigned long)fsz);
        f_close(&s_io_fil);
        return false;
    }

    uint32_t size = (uint32_t)fsz;
    f_lseek(&s_io_fil, header_offset);

    dmg_draw_loading_screen(&s_md_desc, "CARREGANDO ROM", filename, size);

    /* A PSRAM board never programs the ROM into flash: the erase/program cycle
     * on a 4 MB cart takes seconds and burns the flash window the loader image
     * also lives in. Route to PSRAM before the flash probe, which reads
     * MD_ROM_CACHE_ABS and would otherwise compare against stale contents. */
    if (Frens::isPsramEnabled()) {
        bool ok = md_load_rom_to_psram(&s_io_fil, size, filename);
        f_close(&s_io_fil);
        /* The QMI timing was set for flash-XIP execution at the top of this
         * function; with the ROM in PSRAM the emulator runs from there, so put
         * it back the way initAll() left it. */
        apply_safe_qmi_timing();
        if (ok && !dmg_boot_intro_active()) {
            ili9341_blitter_start();
            ili9341_fill_screen(0x0000);
            ili9341_flush(true);
        }
        return ok;
    }

    /* Probe check: see if same ROM is already cached in flash */
    static uint8_t probe_buf[4096];
    UINT got = 0;
    bool already_flashed = false;
    if (f_read(&s_io_fil, probe_buf, sizeof(probe_buf), &got) == FR_OK && got == sizeof(probe_buf)) {
        md_byteswap_words(probe_buf, sizeof(probe_buf));
        already_flashed = (memcmp((const void *)MD_ROM_CACHE_ABS, probe_buf, sizeof(probe_buf)) == 0);
    }
    f_lseek(&s_io_fil, header_offset);

    if (already_flashed) {
        printf("[I] ROM already present in Flash Cache (%u bytes)\n", (unsigned)size);
        f_close(&s_io_fil);
        apply_safe_qmi_timing();
        last_flashed_rom_size = size;
        return true;
    }

    /* Program into Flash XIP */
    printf("[I] Flashing %s (%u bytes) to 0x%08X (offset 0x%08X)...\n",
           filename, (unsigned)size, (unsigned)MD_ROM_CACHE_ABS, (unsigned)MD_ROM_CACHE_OFF);

    static uint8_t copy_block[4096];
    uint32_t offset = 0;
    dmg_boot_intro_flash_begin();   /* core 1 runs the intro from XIP */

    while (offset < size) {
        watchdog_update();
        uint32_t chunk = (size - offset > sizeof(copy_block)) ? sizeof(copy_block) : (size - offset);
        UINT read = 0;
        if (f_read(&s_io_fil, copy_block, chunk, &read) != FR_OK || read != chunk) {
            printf("[E] SD read error at offset %u\n", (unsigned)offset);
            f_close(&s_io_fil);
            dmg_boot_intro_flash_end();
            return false;
        }

        md_byteswap_words(copy_block, chunk);
        const uint32_t programmed = (chunk + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
        if (programmed > chunk) memset(copy_block + chunk, 0xFF, programmed - chunk);

        /* Erase 64KB flash blocks as we cross boundaries */
        if ((offset % FLASH_BLOCK_SIZE) == 0u) {
            safe_flash_range_erase(MD_ROM_CACHE_OFF + offset, FLASH_BLOCK_SIZE);
        }

        for (uint32_t pos = 0; pos < programmed; pos += FLASH_PAGE_SIZE) {
            safe_flash_range_program(MD_ROM_CACHE_OFF + offset + pos, copy_block + pos, FLASH_PAGE_SIZE);
        }

        if (memcmp((const void *)(MD_ROM_CACHE_ABS + offset), copy_block, chunk) != 0) {
            printf("[E] Flash verification failed at offset %u!\n", (unsigned)offset);
            f_close(&s_io_fil);
            dmg_boot_intro_flash_end();
            return false;
        }

        offset += chunk;
        dmg_update_loading_progress(&s_md_desc, offset, size);
    }

    f_close(&s_io_fil);
    dmg_boot_intro_flash_end();
    apply_safe_qmi_timing();
    last_flashed_rom_size = size;
    printf("[I] Flash caching completed successfully\n");
    return true;
}

/* =========================================================================
 * Joypad Input Mapping (Mega Drive 3-Button)
 * ========================================================================= */
static bool s_supress_input = false;

static void update_md_input(void) {
    dmg_pad_t pad_in;
    read_ui_buttons(&pad_in.up, &pad_in.down, &pad_in.left, &pad_in.right,
                    &pad_in.a, &pad_in.b, &pad_in.select, &pad_in.start);

    /* Combos:
     * START + B: Pause Menu
     * START + A: Toggle FPS Overlay
     * SELECT + A: Toggle 1x / 2x speed
     * SELECT + START: Exit to ROM selector (hold ~1 second)
     * SELECT + UP / DOWN: Volume
     *
     * The SELECT+START exit is the one that needs the hold. GP8 is SELECT and it
     * maps to Genesis Button C, so in an action game (Sonic 2 jump, Comix Zone
     * attack) a press of C while START happens to be down would otherwise close
     * the game instantly. 60 consecutive frames of deliberate hold is what makes
     * it unambiguous; the shared layer counts those frames for all nine
     * emulators rather than each keeping its own counter.
     */
    dmg_combo_events_t ev;
    dmg_combo_frame(&pad_in, &s_md_desc, &ev);

    if (ev.request_pause_menu) s_request_pause_menu = true;
    if (ev.request_exit_game) {
        s_request_exit_game = true;
        printf("[I] Combo SELECT+START (held 1s): Exiting to menu\n");
    }
    if (ev.request_power_off) {
        s_request_power_off = true;
        s_power_reason = ev.power_reason;
    }
    if (ev.speed_toggled) {
        printf("[I] Combo SELECT+A: speed = %s\n",
               persistent_turbo_mode ? "2x" : "1x");
    }

    /* s_supress_input is not just "a combo fired this frame": it has to stay
     * latched until every button is released, because the player is still
     * holding the combo on the frames after the edge. Comparing against the raw
     * pad is what detects the release. */
    if (ev.request_pause_menu || ev.request_exit_game ||
        ev.speed_toggled || ev.fps_toggled || dmg_combo_volume_active()) {
        s_supress_input = true;
    }

    if (s_supress_input) {
        if (!pad_in.start && !pad_in.b && !pad_in.select && !pad_in.a &&
            !pad_in.up && !pad_in.down) {
            s_supress_input = false;
        }
        button_state[0] = 0xFF;
        button_state[1] = 0xFF;
        button_state[2] = 0xFF;
        return;
    }

    uint8_t mapped = 0;
    if (pad_in.up)    mapped |= (1u << PAD_UP);
    if (pad_in.down)  mapped |= (1u << PAD_DOWN);
    if (pad_in.left)  mapped |= (1u << PAD_LEFT);
    if (pad_in.right) mapped |= (1u << PAD_RIGHT);
    if (pad_in.b)     mapped |= (1u << PAD_B);      /* GP7 (B) -> Genesis B */
    if (pad_in.select) mapped |= (1u << PAD_C);     /* GP8 (SELECT) -> Genesis C */
    if (pad_in.a)     mapped |= (1u << PAD_A);      /* GP6 (A) -> Genesis A */
    if (pad_in.start) mapped |= (1u << PAD_S);      /* GP9 (START) -> Genesis Start */

    button_state[0] = (unsigned char)~mapped;
    button_state[1] = 0xFF;
    button_state[2] = 0xFF;
}

extern "C" void gwenesis_io_get_buttons(void) {
    /* Inputs are sampled and button_state is updated once per frame in run_game().
     * Do NOT run update_md_input() inside 68k memory read handlers. */
}

/* PSRAM allocation for the C-side port code (buffers.c allocates the YM2612
 * synthesis tables here).
 *
 * This was a stub returning nullptr, so gwsram_ensure_buffer()'s PSRAM
 * fallback never actually ran: on a console build the cart save RAM had no
 * place to go when the SRAM heap was full, and it silently reported "this game
 * will run but cannot save".
 *
 * Frens::f_malloc is the allocator the PSRAM window was registered with (see
 * PicoPlusPsram's constructor calling lwmem_assignmem); plain malloc() would
 * come from the SRAM heap instead, which is the thing we are trying to avoid.
 *
 * One caveat worth keeping: f_malloc enters panic() when the request does not
 * fit, rather than returning NULL, so callers must check the size first --
 * md_load_rom_to_psram() does. */
extern "C" void *gwsram_port_psram_alloc(size_t size) {
    if (!Frens::isPsramEnabled()) {
        return nullptr;
    }
    return Frens::f_malloc(size);
}

extern "C" int gwsram_port_psram_available(void) {
    return Frens::isPsramEnabled() ? 1 : 0;
}

extern "C" void gwsram_port_psram_free(void *p) {
    if (p && Frens::isPsramEnabled()) {
        Frens::f_free(p);
    }
}

/* =========================================================================
 * Main Emulation Execution
 * ========================================================================= */
static void run_game(void) {
    apply_safe_qmi_timing();
    if (last_flashed_rom_size < 0x200) return;

    const uint8_t *rom = s_rom_ptr;
    if (!md_is_valid_rom(rom, last_flashed_rom_size)) {
        printf("[E] Not a valid Genesis ROM (in %s)!\n",
               s_rom_in_psram ? "PSRAM" : "flash");
        return;
    }

    gwsram_detect(rom, last_flashed_rom_size);

    if (!init_emulator_mem()) {
        printf("[E] Failed to allocate emulator memory buffers!\n");
        return;
    }

    load_cartridge(rom, last_flashed_rom_size);
    power_on();
    reset_emulation();
    system_load_sram(selectedRom);

    uint16_t *fb = (uint16_t *)ili9341_get_framebuffer();
    gwenesis_vdp_set_buffer(fb);

    dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */
    ili9341_clear_black();
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_set_interleave(persistent_interleave);
    ili9341_set_fps_overlay(persistent_display_fps, "60");
    ili9341_blitter_start();

    gwsnd_set_output(gwsnd_bridge_push);
    gwsnd_set_bridge_sink(audioOutI2S);
    gwsnd_set_fill_query(audio_i2s_get_fill_permille);
    gwsnd_init(0, 1);
    if (!gwsnd_is_offload_active()) {
        printf("[WARN] Core 1 audio offload inactive, falling back to sync mode\n");
        gwsnd_set_output(audioOutI2S);
    }

    s_request_pause_menu = false;
    s_request_exit_game = false;
    reset = false;
    /* Drop any half-finished SELECT+START hold or closed edge latch left over
     * from the previous game, plus the input suppression that goes with it. */
    dmg_combo_reset();
    s_supress_input = false;

    uint32_t frame_start_us = time_us_32();
    uint32_t fps_timer_us = time_us_32();
    uint32_t frame_count = 0;

    watchdog_enable(6000, false);

    while (!s_request_exit_game) {
        watchdog_update();

        if (reset) {
            reset_emulation();
            reset = false;
        }

        update_md_input();

        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (s_request_power_off) {
            s_request_power_off = false;
            ili9341_blitter_stop();
            dmg_power_off_with_resume(&s_md_desc, (dmg_power_reason_t)s_power_reason);
        }

        if (s_request_pause_menu) {
            s_request_pause_menu = false;
            ingame_action_e action = show_ingame_menu(selectedRom);
            if (action == INGAME_ACTION_EXIT) {
                s_request_exit_game = true;
                break;
            } else if (action == INGAME_ACTION_RESET) {
                reset = true;
            }
            ili9341_clear_black();
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_set_interleave(persistent_interleave);
            gwenesis_vdp_set_buffer(fb);
            ili9341_blitter_start();
            watchdog_update();
            frame_start_us = time_us_32();
        }

        /* Gwenesis Frame Step */
        const bool is_pal = (gwenesis_vdp_status & 1) != 0;
        gwsnd_set_pal(is_pal ? 1 : 0);
        screen_height = REG1_PAL ? 240 : 224;
        screen_width = (persistent_force_h32 || !REG12_MODE_H40) ? 256 : 320;
        const int lines_per_frame = is_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        /* Frameskip evaluation */
        static bool s_skip_next_render = false;
        static bool s_skip_phase = false;
        bool render_this_frame = true;

        if (persistent_frameskip_mode == FRAMESKIP_ON) {
            s_skip_phase = !s_skip_phase;
            render_this_frame = !s_skip_phase;
        } else if (persistent_frameskip_mode == FRAMESKIP_AUTO) {
            if (s_skip_next_render) {
                render_this_frame = false;
                s_skip_next_render = false;
            } else {
                render_this_frame = true;
            }
        } else {
            render_this_frame = true;
        }

        gwenesis_vdp_render_config();
        ili9341_md_set_geometry(screen_width, screen_height);
        system_clock = 0;
        zclk = 0;
        scan_line = 0;

        while (scan_line < lines_per_frame) {
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            z80_run(system_clock + VDP_CYCLES_PER_LINE);
            gwsnd_line_tick(system_clock + VDP_CYCLES_PER_LINE);

            if (scan_line < screen_height) {
                if (render_this_frame && md_line_active[scan_line]) {
                    gwenesis_vdp_render_line(scan_line);
                    ili9341_stream_md_line_fast(md_line_dst_row[scan_line], &fb[scan_line * 320]);
                }
            }

            /* On line 0, reload counter. On VBLANK lines (>= screen_height), reload counter and NEVER fire H-INT! */
            if (scan_line == 0 || scan_line >= screen_height) {
                hint_counter = REG10_LINE_COUNTER;
            } else {
                /* Active display scanlines 1 to screen_height - 1 */
                if (--hint_counter < 0) {
                    if (REG0_LINE_INTERRUPT != 0) {
                        hint_pending = 1;
                        if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                            m68k_update_irq(4);
                    }
                    hint_counter = REG10_LINE_COUNTER;
                }
            }

            scan_line++;
            if (scan_line == screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                z80_irq_line(1);
            }
            if (scan_line == screen_height + 1) {
                z80_irq_line(0);
            }

            system_clock += VDP_CYCLES_PER_LINE;
        }

        if (render_this_frame) {
            ili9341_stream_md_frame_end();
        }
        gwsnd_frame_end(system_clock);
        m68k.cycles -= system_clock;
        frame_counter++;
        frame_count++;

        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            ili9341_blitter_stop();
            const bool resumed = md_load_state(selectedRom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_clear_black();
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_set_interleave(persistent_interleave);
            gwenesis_vdp_set_buffer(fb);
            ili9341_blitter_start();
            watchdog_update();
            frame_start_us = time_us_32();
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }

        /* FPS calculation */
        uint32_t now = time_us_32();
        if (now - fps_timer_us >= 1000000u) {
            fps = frame_count;
            frame_count = 0;
            fps_timer_us = now;
            snprintf(fpsString, sizeof(fpsString), "%lu", (unsigned long)fps);
            if (persistent_display_fps) {
                ili9341_set_fps_overlay(true, fpsString);
            }
        }

        /* Frame Pacing (59.92 Hz NTSC / 50 Hz PAL) */
        uint32_t frame_budget = is_pal ? 20000u : 16687u;
        if (persistent_turbo_mode) frame_budget /= 2;

        uint32_t elapsed = time_us_32() - frame_start_us;
        if (elapsed < frame_budget) {
            busy_wait_us_32(frame_budget - elapsed);
        }

        /* Adaptive auto-frameskip safety net */
        static bool s_last_frame_skipped = false;
        if (persistent_frameskip_mode == FRAMESKIP_AUTO) {
            bool need_skip = false;
            int fill = audio_i2s_get_fill_permille();
            if (!persistent_turbo_mode) {
                if (persistent_volume_level > 0 && fill < 100) {
                    need_skip = true;
                } else if (!s_last_frame_skipped && (elapsed >= (is_pal ? 24000u : 18500u))) {
                    need_skip = true;
                }
            }
            if (need_skip) {
                s_skip_next_render = true;
                s_last_frame_skipped = true;
            } else {
                s_skip_next_render = false;
                s_last_frame_skipped = false;
            }
        } else {
            s_skip_next_render = false;
            s_last_frame_skipped = false;
        }
        frame_start_us = time_us_32();
    }

    watchdog_disable();

    gwsnd_shutdown();
    ili9341_blitter_stop();
    system_save_sram(selectedRom);
    free_emulator_mem();
    ili9341_clear_black();
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */
int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_35);
    set_sys_clock_khz(CPUFreqKHz, true);
    apply_safe_qmi_timing();

    /* Decouple SPI peripheral clock from CPU overclock (378 MHz):
     * Route clk_peri to PLL_USB at 144 MHz (VCO 864 MHz, postdiv 6*1 = 6)
     * so divisor 2 gives an exact 72.0 MHz for display, and standard <= 20 MHz for SD card. */
    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
        clock_get_hz(clk_sys),
        clock_get_hz(clk_sys)
    );
    pll_init(pll_usb, 1, 864 * MHZ, 6, 1);
    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
        144 * MHZ,
        144 * MHZ
    );

    stdio_init_all();
    init_ui_buttons();

    selectedRom[0] = '\0';
    ErrorMessage[0] = '\0';

    FrensSettings::initSettings(FrensSettings::emulators::GENESIS);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, 1024, false, true);

#if USE_ILI9341
    ili9341_init();
    dmg_init_boot_mode();
    load_emulator_config();
    set_backlight_brightness(persistent_brightness_level);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_set_interleave(persistent_interleave);
    ili9341_clear_black();
#endif

    gwsnd_set_output(audioOutI2S);
    ensure_save_directories();

    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_md_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_md_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_md_desc);
            snprintf(selectedRom, sizeof(selectedRom), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_md_desc)) dmg_boot_intro_finish();
        }
    }

    printf("\n==================================================\n");
    printf("  %s Sega Mega Drive / Genesis for RP2350\n", APP_TITLE);
    printf("  CPU: %lu MHz | Display: ILI9341 (320x240 @ 60 FPS)\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    printf("==================================================\n\n");

    while (true) {
        if (s_resume_pending || rom_file_selector(selectedRom, sizeof(selectedRom))) {
            if (load_cart_rom_file(selectedRom)) {
                /* The config file is the source of truth for how the game is
                 * displayed, so it is re-read when a game is entered, not only at
                 * boot. Safe to repeat because every menu visit commits first, so
                 * the file always matches what was on screen when the menu closed.
                 */
                load_emulator_config();
                set_backlight_brightness(persistent_brightness_level);
                ili9341_set_scale_mode(persistent_scale_mode);
                ili9341_set_scanlines(persistent_scanlines_enabled);
                ili9341_set_shimmerless(persistent_shimmerless_enabled);
                ili9341_set_interleave(persistent_interleave);
                dmg_power_set_game(selectedRom, selectedRom);
                run_game();
                dmg_power_set_game(NULL, NULL);
                if (s_resume_pending) {   /* run_game() bailed before the state */
                    s_resume_pending = false;
                    dmg_resume_consume();
                }
                dmg_boot_intro_finish();
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

    return 0;
}
