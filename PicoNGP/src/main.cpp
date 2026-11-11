/**
 * PicoNGP for Raspberry Pi RP2350 + ILI9341 SPI TFT + MAX98357A I2S
 *
 * SNK NeoGeo Pocket / NeoGeo Pocket Color Emulator with Clean Light UI,
 * In-Game Pause Menu, Save/Load States (4 slots), Grid Image Effect,
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

#if USE_ILI9341
#include "ili9341.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#endif

extern "C" {
#include "neopop/neopop.h"
#include "neopop/ngp_prof.h"
#include "neopop/sound.h"
#include "neopop/flash.h"
#include "neopop/state.h"
#include "neopop/mem.h"
#include "neopop/gfx.h"
#include "neopop/interrupt.h"
#include "neopop/bios.h"
}

/* =========================================================================
 * Build Target Configuration (PicoNGP)
 * ========================================================================= */
#define APP_TITLE            "PicoNGP"
#define CONFIG_MAGIC         0x4E475053u /* "NGPS" */
#define CONFIG_PATH          "0:/saves/ngp/picongp.cfg"
#define CONFIG_PATH_FALLBACK "/saves/ngp/picongp.cfg"
#define SAVES_DIR            "0:/saves/ngp"
#define SAVES_DIR_FALLBACK   "/saves/ngp"
#define ROMS_DIR             "0:/roms/ngp"
#define ROMS_DIR_FALLBACK    "/roms/ngp"

#define NGP_ROM_CACHE_HEADROOM 0x100000u /* 1 MB headroom beyond flash code */
#define NGP_ROM_CACHE_MAX      0x400000u /* 4 MB ROM cache max */
#define NGP_ROM_CACHE_ABS      (0x10080000u + NGP_ROM_CACHE_HEADROOM)
#define NGP_ROM_CACHE_OFF      (NGP_ROM_CACHE_ABS - 0x10000000u)

#define NGP_CACHE_MAGIC        0x4E475043u /* "NGPC" */
#define NGP_CACHE_HDR_OFF      (NGP_ROM_CACHE_OFF - FLASH_SECTOR_SIZE)
#define NGP_CACHE_HDR_ABS      (NGP_ROM_CACHE_ABS - FLASH_SECTOR_SIZE)

typedef struct __attribute__((aligned(4))) {
    uint32_t magic;          /* NGP_CACHE_MAGIC */
    uint32_t rom_size;       /* ROM size in bytes */
    uint32_t sample_crc;     /* FNV hash over 32 spot sample blocks */
    char     filename[64];   /* Sanitized ROM filename */
    uint8_t  padding[256 - 76]; /* Pad to full 256-byte flash page */
} PicoNGP_Cache_Header_t;

static uint32_t compute_ngp_sample_hash(const uint8_t *rom_base, uint32_t size) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 32; i++) {
        uint32_t sample_offset = (size > 512) ? (uint32_t)(((uint64_t)i * (size - 512)) / 31) : 0;
        const uint32_t *p = (const uint32_t *)(rom_base + sample_offset);
        for (int w = 0; w < 128; w++) {
            hash = (hash ^ p[w]) * 16777619u;
        }
    }
    return hash;
}

#if PICO_RP2350
#define EMULATOR_CLOCKFREQ_KHZ 360000
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

/* Where the running cartridge lives.
 *
 * On the console build (PSRAM_CS_PIN != 0) the ROM is read straight from the SD
 * card into PSRAM and s_rom_ptr points there; the HW15 build has no PSRAM and
 * programs it into the flash XIP window at NGP_ROM_CACHE_ABS instead, leaving
 * s_rom_ptr NULL. A flag rather than an address-range test, because a PSRAM
 * pointer is in the 0x8... range and the XIP window is a perfectly legal-looking
 * 0x1018.... */
static uint8_t *s_rom_ptr = NULL;
static bool s_rom_in_psram = false;

static uint32_t fps = 0;
static char fpsString[8] = "60";

extern "C" volatile bool s_frame_completed;
volatile bool s_frame_completed = false;

/* =========================================================================
 * Persistent Configuration Structure
 * ========================================================================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t scale_mode;        /* 0 = Fit, 1 = Stretch, 2 = Integer */
    uint32_t effect_enabled;    /* 0 = None, 1 = Grid */
    uint32_t display_fps;       /* 0 = Off, 1 = On */
    uint32_t volume_level;      /* 1..5, default 3 */
    uint32_t brightness_level;  /* 1..5, default 5 */
    uint32_t turbo_mode;        /* 0 = Normal 1x, 1 = Turbo 2x */
    uint32_t save_slot;         /* 1..4, default 1 */
    uint32_t load_slot;         /* 1..4, default 1 */
    uint32_t interleave;        /* 0 = Progressive, 1 = Interleave */
} emulator_config_t;

static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_grid_effect = false;
static bool persistent_interleave = true;       /* 0 = Progressive, 1 = Interleave (default 60 FPS) */
static bool persistent_display_fps = false;
static uint8_t persistent_volume_level = 3;     /* 1..5 */
static uint8_t persistent_brightness_level = 5; /* 1..5 */
static uint8_t persistent_turbo_mode = 0;       /* 0 = 1x, 1 = 2x */
static uint8_t persistent_save_slot = 1;        /* 1..4 */
static uint8_t persistent_load_slot = 1;        /* 1..4 */
static bool persistent_shimmerless_enabled = true;

static bool s_request_pause_menu = false;
static bool s_request_exit_game = false;
static bool reset_requested = false;

/* Static buffers to prevent stack overflow */
static char current_rom_dir[64] = ROMS_DIR;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;
static char s_full_path[FF_MAX_LFN + 32];
static FIL  s_io_fil;

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
#define draw_clean_light_top_bar(t, c, p) dmg_draw_top_bar(&s_ngp_desc, t, "")
#define draw_clean_light_bottom_bar(tf)   dmg_draw_bottom_bar(&s_ngp_desc, "", "")

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

extern const dmg_emulator_desc_t s_ngp_desc;
#define save_emulator_config() ngp_save_config()
#define load_emulator_config() ngp_load_config()

/* =========================================================================
 * Path Helpers (Save States & Battery Saves)
 * ========================================================================= */
static void get_sanitized_basename(const char *game_name, char *out, size_t out_len) {
    const char *slash = strrchr(game_name, '/');
    if (!slash) slash = strrchr(game_name, '\\');
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

static void build_state_path(const char *game_name, uint8_t slot, char *out, size_t out_len) {
    char base[64];
    get_sanitized_basename(game_name, base, sizeof(base));
    snprintf(out, out_len, "%s/%s_%u.ss", SAVES_DIR, base, (unsigned)slot);
}

static void build_save_path(const char *game_name, char *out, size_t max_len, bool fallback = false) {
    char base[64];
    get_sanitized_basename(game_name, base, sizeof(base));
    snprintf(out, max_len, "%s/%s.sav", fallback ? SAVES_DIR_FALLBACK : SAVES_DIR, base);
}

static void build_save_path_raw(const char *game_name, char *out, size_t max_len, bool fallback = false) {
    const char *slash = strrchr(game_name, '/');
    if (!slash) slash = strrchr(game_name, '\\');
    const char *src = slash ? (slash + 1) : game_name;
    char base[64];
    strncpy(base, src, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    snprintf(out, max_len, "%s/%s.sav", fallback ? SAVES_DIR_FALLBACK : SAVES_DIR, base);
}

/* =========================================================================
 * Audio Processing (I2S Output at 44.1 kHz)
 * ========================================================================= */
#define AUDIO_SAMPLE_RATE 44100
static int16_t s_chip_audio_buf[735];
static _u8     s_dac_audio_buf[735];
static uint32_t s_audio_out_buf[735];

static inline void update_audio_frame(void) {
    static const int32_t vol_tbl[6] = { 0, 51, 102, 154, 205, 256 };
    int32_t vol = vol_tbl[persistent_volume_level > 5 ? 5 : persistent_volume_level];

    sound_update((_u16*)s_chip_audio_buf, 735 * sizeof(int16_t));
    dac_update(s_dac_audio_buf, 735);

    if (vol <= 0) {
        memset(s_audio_out_buf, 0, 735 * sizeof(uint32_t));
        EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_out_buf, 735);
        return;
    }

    const int16_t * __restrict chip = s_chip_audio_buf;
    const _u8     * __restrict dac  = s_dac_audio_buf;
    uint32_t      * __restrict out  = s_audio_out_buf;

    for (int i = 0; i < 735; i++) {
        int32_t s = (int32_t)chip[i];
        int32_t dac_s = ((int32_t)dac[i] - 128) << 8;
        s += dac_s;

        s = (s * vol) >> 8;
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;

        uint16_t s16 = (uint16_t)(int16_t)s;
        out[i] = ((uint32_t)s16 << 16) | s16;
    }
    EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_out_buf, 735);
}


typedef dmg_action_t ingame_action_e;
#define INGAME_ACTION_RESUME DMG_ACTION_RESUME
#define INGAME_ACTION_RESET  DMG_ACTION_RESET
#define INGAME_ACTION_EXIT   DMG_ACTION_EXIT

static const char *s_ngp_candidate_dirs[] = {
    "0:/roms/ngp",
    "/roms/ngp",
    "roms/ngp",
    "0:/ROMS/NGP",
    "/ROMS/NGP",
    "ROMS/NGP",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};

static bool is_ngp_rom(const char *name) {
    if (!name || name[0] == '\0') return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".ngc") == 0 ||
            strcasecmp(dot, ".ngpc") == 0 ||
            strcasecmp(dot, ".ngp") == 0 ||
            strcasecmp(dot, ".npc") == 0 ||
            strcasecmp(dot, ".bin") == 0);
}

static bool ngp_save_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    ensure_save_directories();
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));
    state_store(path);
    return true;
}

static bool ngp_load_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));
    FILINFO fno;
    bool exists = (f_stat(path, &fno) == FR_OK && fno.fsize > 0);
    if (!exists && slot == 1) {
        char fallback_path[FF_MAX_LFN];
        char base[64];
        dmg_sanitize_basename(game_name, base, sizeof(base));
        snprintf(fallback_path, sizeof(fallback_path), "%s/%s.ss", SAVES_DIR, base);
        if (f_stat(fallback_path, &fno) == FR_OK && fno.fsize > 0) {
            strncpy(path, fallback_path, sizeof(path) - 1);
            exists = true;
        }
    }
    if (exists) {
        state_restore(path);
        return true;
    }
    return false;
}

static void ngp_apply_scale(dmg_scale_mode_t mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void ngp_apply_effect(bool effect) {
    persistent_grid_effect = effect;
    ili9341_set_grid_effect(persistent_grid_effect);
}

static void ngp_apply_interleave(bool interleave) {
    persistent_interleave = interleave;
    ili9341_set_interleave(persistent_interleave);
}

static void ngp_apply_speed(bool turbo) {
    persistent_turbo_mode = turbo ? 1 : 0;
    /* Turbo drops every other emulated frame, so the display has to switch to
     * whole frames or the interleave field flip shows up as a comb of
     * half-updated scanlines. See ili9341_set_fast_forward(). */
    ili9341_set_fast_forward(persistent_turbo_mode != 0u);
}

static void ngp_apply_volume(uint8_t vol) {
    set_sound_volume_level(vol);
}

static void ngp_apply_brightness(uint8_t br) {
    set_backlight_brightness(br);
}

static void ngp_apply_filter(bool shimmerless) {
    persistent_shimmerless_enabled = shimmerless;
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

/* Combo hooks, defined after the descriptor they are stored in. */
static void ngp_save_config(void);
static bool ngp_get_fps(void);
static void ngp_apply_fps(bool on);

static bool ngp_on_load_rom(const struct dmg_emulator_desc *desc, const char *filename) {
    (void)desc;
    (void)filename;
    const char *dir = dmg_get_current_rom_dir();
    if (dir && dir[0]) {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dir);
    }
    return true;
}

static void draw_ngp_image_row(uint8_t idx, bool selected) {
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
        uint8_t sm = (uint8_t)persistent_scale_mode;
        if (sm > 2) sm = 0;
        snprintf(buf, sizeof(buf), "Scale:        %s", scale_names[sm]);
    } else if (idx == 1) {
        snprintf(buf, sizeof(buf), "Effect:       %s", persistent_grid_effect ? "< Grid >" : "< None >");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Filter:       %s", persistent_shimmerless_enabled ? "< Shimmerless >" : "< Sharp >");
    } else if (idx == 3) {
        snprintf(buf, sizeof(buf), "Screen:       %s", persistent_interleave ? "< Interleave >" : "< Progressive >");
    }

    ili9341_text(buf, wx + 12, (uint16_t)(y + 7u), selected ? DMG_RGB565_TEXT_SEL_COLOR : DMG_RGB565_TEXT_COLOR, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void ngp_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;
    bool changed = false;

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
        draw_ngp_image_row(i, i == cursor);
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
            draw_ngp_image_row(prev, false);
            draw_ngp_image_row(cursor, true);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == 3) ? 0 : (cursor + 1);
            draw_ngp_image_row(prev, false);
            draw_ngp_image_row(cursor, true);
        }

        if (pushed_left || pushed_right || pushed_a) {
            if (cursor == 0) {
                if (pushed_right || pushed_a) {
                    persistent_scale_mode = (ili9341_scale_mode_t)(((uint8_t)persistent_scale_mode + 1u) % 3u);
                } else {
                    persistent_scale_mode = (ili9341_scale_mode_t)((persistent_scale_mode == 0) ? 2 : ((uint8_t)persistent_scale_mode - 1u));
                }
                draw_ngp_image_row(0, true);
                changed = true;
            } else if (cursor == 1) {
                persistent_grid_effect = !persistent_grid_effect;
                draw_ngp_image_row(1, true);
                changed = true;
            } else if (cursor == 2) {
                persistent_shimmerless_enabled = !persistent_shimmerless_enabled;
                draw_ngp_image_row(2, true);
                changed = true;
            } else if (cursor == 3) {
                persistent_interleave = !persistent_interleave;
                draw_ngp_image_row(3, true);
                changed = true;
            }
        }

        if (pushed_b) {
            if (changed) {
                ili9341_set_scale_mode(persistent_scale_mode);
                ili9341_set_grid_effect(persistent_grid_effect);
                ili9341_set_shimmerless(persistent_shimmerless_enabled);
                ili9341_set_interleave(persistent_interleave);
                ngp_save_config();
            }
            dmg_wait_for_all_buttons_released();
            return;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}

#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
/* Resume intro chime, pushed from core 1 into the I2S ring (dmg_boot_intro.c). */
static int  ngp_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void ngp_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void ngp_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(44100);
}
#endif

/* Cartridge flash save to the SD before a standby (dmg_power_off_with_resume). */
static void ngp_flush_saves(void) {
    if (flash_is_dirty()) flash_commit();
}

const dmg_emulator_desc_t s_ngp_desc = {
    .app_title            = APP_TITLE,
    .default_rom_dir      = ROMS_DIR,
    .candidate_dirs       = s_ngp_candidate_dirs,
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
    .is_rom_file          = is_ngp_rom,
    .on_load_rom          = ngp_on_load_rom,
    .save_state           = ngp_save_state,
    .load_state           = ngp_load_state,
    .apply_scale          = ngp_apply_scale,
    .apply_effect         = ngp_apply_effect,
    .apply_interleave     = ngp_apply_interleave,
    .apply_speed          = ngp_apply_speed,
    .apply_volume         = ngp_apply_volume,
    .apply_brightness     = ngp_apply_brightness,
    .apply_filter         = ngp_apply_filter,
    .reset_game           = NULL,
    .exit_game            = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu       = ngp_image_submenu,
    .save_config          = ngp_save_config,
    .get_fps              = ngp_get_fps,
    .apply_fps            = ngp_apply_fps,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_free           = ngp_audio_free,
    .audio_push           = ngp_audio_push,
#endif
    .flush_saves          = ngp_flush_saves,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_restore        = ngp_audio_restore
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
static void ngp_save_config(void) {
    g_dmg_config.scale_mode       = (uint32_t)persistent_scale_mode;
    g_dmg_config.grid_effect      = persistent_grid_effect ? 1u : 0u;
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
    dmg_config_save(&s_ngp_desc);
}

static void ngp_load_config(void) {
    /* dmg_config_load() pushes scale/effect/interleave/speed/volume/brightness
     * back into the persistent_* variables through the descriptor callbacks.
     * extra_opt1 has no callback of its own, so the FPS overlay is read and
     * applied by hand. */
    dmg_config_load(&s_ngp_desc);
    persistent_display_fps = (g_dmg_config.extra_opt1 != 0u);
    if (s_ngp_desc.apply_fps != NULL) s_ngp_desc.apply_fps(persistent_display_fps);
    persistent_shimmerless_enabled = (g_dmg_config.extra_opt2 == 0u);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}


static bool ngp_get_fps(void) {
    return persistent_display_fps;
}

static void ngp_apply_fps(bool on) {
    persistent_display_fps = on;
    ili9341_set_fps_overlay(persistent_display_fps, fpsString);
}

static bool rom_file_selector(char *selected_out, size_t max_len) {
    return dmg_rom_selector(&s_ngp_desc, selected_out, max_len);
}

static ingame_action_e show_ingame_menu(const char *game_name) {
    return dmg_ingame_menu(&s_ngp_desc, game_name);
}

/* =========================================================================
 * Flash Caching & ROM Loading
 * ========================================================================= */

/* Reads the cartridge from the SD card straight into PSRAM.
 *
 * This replaces the flash XIP path on the console build, and the reason is
 * visible in the numbers rather than in principle:
 *
 *  - The HW15 build programs the ROM into a 4 MB window at 0x10180000. Every
 *    load erases and rewrites megabytes of QSPI, which is what the persistent
 *    cache header and the 32-block spot-check exist to avoid, and what pushes
 *    past the 6 s watchdog on the larger carts.
 *  - The HW15 build has 22.872 bytes of SRAM free (95.64% used), below the
 *    40-50 KB its own agents.md reserves for the stack and the FatFs heap.
 *    Moving the ROM out of the flash window is what makes room to grow.
 *
 * The image is byte-identical to what the flash path would have held, so the
 * core's rom.data contract is unchanged: the raw file is loaded at offset 0 and
 * mem.h addresses it from ROM_START exactly as before.
 *
 * Allocation goes through Frens::f_malloc (the allocator the PSRAM window was
 * registered with) and release through Frens::f_free. Never free() on these
 * blocks: plain free() comes from the SRAM heap, and handing it a lwmem block
 * corrupts the heap and hangs the MCU on the way out of a game. */
static bool ngp_load_rom_to_psram(FIL *fil, uint32_t size, const char *filename) {
    if (size == 0) {
        printf("[E] Refusing to load a zero-byte ROM into PSRAM\n");
        return false;
    }

    /* f_malloc panics on failure, so the size has to be checked up front rather
     * than discovered by the allocator -- panic is an infinite loop, and it
     * presents as a black screen after the ROM loads. The margin covers the
     * menu's own scratch allocations plus lwmem bookkeeping. */
    const uint32_t margin = 512u * 1024u;
    const uint32_t avail = (uint32_t)Frens::GetAvailableMemory();
    if (avail > margin && size > (avail - margin)) {
        printf("[E] ROM needs %u bytes, only %u usable in PSRAM\n",
               (unsigned)size, (unsigned)(avail - margin));
        return false;
    }

    /* Release the previous game before allocating, otherwise switching between
     * ROMs leaks the whole of each one and eventually starves the allocator. */
    if (s_rom_in_psram && s_rom_ptr) {
        printf("[I] Freeing previous PSRAM ROM at 0x%08x\n",
               (unsigned)(uintptr_t)s_rom_ptr);
        Frens::f_free(s_rom_ptr);
        s_rom_ptr = NULL;
        s_rom_in_psram = false;
    }

    uint8_t *buf = (uint8_t *)Frens::f_malloc(size);
    if (!buf) {
        printf("[E] PSRAM allocation of %u bytes failed\n", (unsigned)size);
        return false;
    }

    /* Published before the read so a failed load is still recognisable. */
    s_rom_ptr = buf;
    s_rom_in_psram = true;

    char fname_disp[44];
    get_sanitized_basename(filename, fname_disp, sizeof(fname_disp));
    for (char *p = fname_disp; *p != '\0'; p++) {
        if (*p == '_') *p = ' ';
    }
    dmg_draw_loading_screen(&s_ngp_desc, "CARREGANDO NGP ROM", fname_disp, size);

    /* Read in chunks so the progress bar reflects real progress, and so a short
     * read stops the loop instead of leaving the tail of the buffer
     * uninitialised -- NeoPop would then execute whatever was in PSRAM. */
    const uint32_t PSRAM_READ_CHUNK = 16u * 1024u;
    uint32_t bytes_loaded = 0;
    bool ok = true;
    bool led_on = false;
    f_lseek(fil, 0);

    while (bytes_loaded < size) {
        watchdog_update();
        led_on = !led_on;
        Frens::blinkLed(led_on);

        uint32_t want = size - bytes_loaded;
        if (want > PSRAM_READ_CHUNK) want = PSRAM_READ_CHUNK;
        UINT br = 0;
        FRESULT fr = f_read(fil, buf + bytes_loaded, want, &br);
        if (fr != FR_OK || br == 0) {
            printf("[E] f_read failed at %u/%u: %d\n",
                   (unsigned)bytes_loaded, (unsigned)size, (int)fr);
            ok = false;
            break;
        }
        bytes_loaded += br;

        dmg_update_loading_progress(&s_ngp_desc, bytes_loaded, size);
    }

    Frens::blinkLed(false);
    f_lseek(fil, 0);

    if (!ok) {
        Frens::f_free(buf);
        s_rom_ptr = NULL;
        s_rom_in_psram = false;
        return false;
    }

    printf("[I] ROM in PSRAM at 0x%08x: %u bytes\n",
           (unsigned)(uintptr_t)buf, (unsigned)bytes_loaded);
    last_flashed_rom_size = bytes_loaded;
    return true;
}

static bool load_cart_rom_file(const char *filename) {
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
    if (fsz < 512 || fsz > NGP_ROM_CACHE_MAX) {
        printf("[E] Invalid ROM size: %lu\n", (unsigned long)fsz);
        f_close(&s_io_fil);
        return false;
    }

    uint32_t size = (uint32_t)fsz;

    /* Console build: read the ROM straight into PSRAM. Placed before the cache
     * header check because that header describes the flash window, which this
     * path never writes. */
    if (Frens::isPsramEnabled()) {
        return ngp_load_rom_to_psram(&s_io_fil, size, filename);
    }

    /* Fast, rigorous integrity check: verify cache header and 32 distributed samples */
    char clean_name[64];
    get_sanitized_basename(filename, clean_name, sizeof(clean_name));

    const PicoNGP_Cache_Header_t *hdr = (const PicoNGP_Cache_Header_t *)NGP_CACHE_HDR_ABS;
    bool already_flashed = false;

    if (hdr->magic == NGP_CACHE_MAGIC &&
        hdr->rom_size == size &&
        strncmp(hdr->filename, clean_name, sizeof(clean_name)) == 0)
    {
        /* Header matches this exact ROM and size! Spot-check 32 sectors across the file */
        static uint8_t sample_buf[512];
        bool samples_ok = true;
        uint32_t computed_hash = 2166136261u;

        for (int i = 0; i < 32; i++) {
            uint32_t sample_offset = (size > 512) ? (uint32_t)(((uint64_t)i * (size - 512)) / 31) : 0;
            if (f_lseek(&s_io_fil, sample_offset) != FR_OK) {
                samples_ok = false;
                break;
            }
            UINT br = 0;
            if (f_read(&s_io_fil, sample_buf, 512, &br) != FR_OK || br != 512) {
                samples_ok = false;
                break;
            }
            if (memcmp((const void *)(NGP_ROM_CACHE_ABS + sample_offset), sample_buf, 512) != 0) {
                samples_ok = false;
                break;
            }
            const uint32_t *p = (const uint32_t *)sample_buf;
            for (int w = 0; w < 128; w++) {
                computed_hash = (computed_hash ^ p[w]) * 16777619u;
            }
        }

        if (samples_ok && computed_hash == hdr->sample_crc) {
            already_flashed = true;
        }
    }
    f_lseek(&s_io_fil, 0);

    if (already_flashed) {
        printf("[I] ROM verified intact in Flash Cache (%u bytes, 32/32 sample blocks OK)\n", (unsigned)size);
        char fname_disp[44];
        get_sanitized_basename(filename, fname_disp, sizeof(fname_disp));
        for (char *p = fname_disp; *p != '\0'; p++) {
            if (*p == '_') *p = ' ';
        }
        dmg_draw_loading_screen(&s_ngp_desc, "CARREGANDO NGP ROM", fname_disp, size);
        dmg_update_loading_progress(&s_ngp_desc, size, size);
        sleep_ms(150);

        f_close(&s_io_fil);
        apply_safe_qmi_timing();
        last_flashed_rom_size = size;
        return true;
    }

    /* Program into Flash XIP with UI Progress Bar (PicoSNES visual style) */
    printf("[I] Flashing %s (%u bytes) to 0x%08X (offset 0x%08X)...\n",
           filename, (unsigned)size, (unsigned)NGP_ROM_CACHE_ABS, (unsigned)NGP_ROM_CACHE_OFF);

    char fname_disp[44];
    get_sanitized_basename(filename, fname_disp, sizeof(fname_disp));
    for (char *p = fname_disp; *p != '\0'; p++) {
        if (*p == '_') *p = ' ';
    }
    dmg_draw_loading_screen(&s_ngp_desc, "GRAVANDO FLASH CACHE", fname_disp, size);
    dmg_boot_intro_flash_begin();

    static uint8_t copy_block[4096];
    uint32_t offset = 0;
    bool led_on = false;

    while (offset < size) {
        watchdog_update();
        led_on = !led_on;
        Frens::blinkLed(led_on);

        uint32_t chunk = (size - offset > sizeof(copy_block)) ? sizeof(copy_block) : (size - offset);
        UINT read = 0;
        if (f_read(&s_io_fil, copy_block, chunk, &read) != FR_OK || read != chunk) {
            printf("[E] SD read error at offset %u\n", (unsigned)offset);
            Frens::blinkLed(false);
            f_close(&s_io_fil);
            dmg_boot_intro_flash_end();
            return false;
        }

        const uint32_t programmed = (chunk + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
        if (programmed > chunk) memset(copy_block + chunk, 0xFF, programmed - chunk);

        /* Erase 64KB flash blocks as we cross boundaries */
        if ((offset % FLASH_BLOCK_SIZE) == 0u) {
            safe_flash_range_erase(NGP_ROM_CACHE_OFF + offset, FLASH_BLOCK_SIZE);
        }

        for (uint32_t pos = 0; pos < programmed; pos += FLASH_PAGE_SIZE) {
            safe_flash_range_program(NGP_ROM_CACHE_OFF + offset + pos, copy_block + pos, FLASH_PAGE_SIZE);
        }

        if (memcmp((const void *)(NGP_ROM_CACHE_ABS + offset), copy_block, chunk) != 0) {
            printf("[E] Flash verification failed at offset %u!\n", (unsigned)offset);
            Frens::blinkLed(false);
            f_close(&s_io_fil);
            dmg_boot_intro_flash_end();
            return false;
        }

        offset += chunk;
        dmg_update_loading_progress(&s_ngp_desc, offset, size);
    }

    /* Compute sample CRC and write cache header to Flash */
    uint32_t sample_crc = compute_ngp_sample_hash((const uint8_t *)NGP_ROM_CACHE_ABS, size);

    PicoNGP_Cache_Header_t new_hdr;
    memset(&new_hdr, 0, sizeof(new_hdr));
    new_hdr.magic = NGP_CACHE_MAGIC;
    new_hdr.rom_size = size;
    new_hdr.sample_crc = sample_crc;
    strncpy(new_hdr.filename, clean_name, sizeof(new_hdr.filename) - 1);

    safe_flash_range_erase(NGP_CACHE_HDR_OFF, FLASH_SECTOR_SIZE);
    safe_flash_range_program(NGP_CACHE_HDR_OFF, (const uint8_t *)&new_hdr, sizeof(new_hdr));
    dmg_boot_intro_flash_end();

    Frens::blinkLed(false);
    f_close(&s_io_fil);
    apply_safe_qmi_timing();
    last_flashed_rom_size = size;
    printf("[I] Flash caching completed successfully (%u bytes, crc 0x%08X)\n",
           (unsigned)size, (unsigned)sample_crc);
    return true;
}

/* =========================================================================
 * Joypad Input Mapping (NeoGeo Pocket Active-Low GPIO 2..9)
 * ========================================================================= */
static bool s_btn_up = false, s_btn_down = false, s_btn_left = false, s_btn_right = false;
static bool s_btn_a = false, s_btn_b = false, s_btn_start = false;

static void update_ngp_input(void) {
    dmg_pad_t pad_in;
    read_ui_buttons(&pad_in.up, &pad_in.down, &pad_in.left, &pad_in.right,
                    &pad_in.a, &pad_in.b, &pad_in.select, &pad_in.start);

    /* Combos:
     * START + B: In-game pause menu
     * START + A: Toggle FPS overlay
     * SELECT + A: Toggle fast-forward 2x
     * SELECT + START: Exit to ROM selector (held ~1 s)
     * SELECT + UP / DOWN: Volume adjustment
     *
     * The pad comes back already masked, so neither SELECT nor A reaches the
     * NeoGeo Pocket as a stray press -- A is a face button there, so a leak
     * would be a visible jump or fire rather than a silent one. */
    dmg_combo_events_t ev;
    dmg_combo_frame(&pad_in, &s_ngp_desc, &ev);

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
        printf("[I] Fast-forward %s\n", persistent_turbo_mode ? "2x ON" : "1x");
    }

    s_btn_up = pad_in.up;
    s_btn_down = pad_in.down;
    s_btn_left = pad_in.left;
    s_btn_right = pad_in.right;
    s_btn_a = pad_in.a;
    s_btn_b = pad_in.b;
    s_btn_start = pad_in.start;
}

/* =========================================================================
 * NeoPop System Callbacks
 * ========================================================================= */
extern "C" {

void system_VBL(void) {
    uint8_t buttons = 0;
    if (s_btn_up)    buttons |= 0x01;
    if (s_btn_down)  buttons |= 0x02;
    if (s_btn_left)  buttons |= 0x04;
    if (s_btn_right) buttons |= 0x08;
    if (s_btn_a)     buttons |= 0x10;
    if (s_btn_b)     buttons |= 0x20;
    if (s_btn_start) buttons |= 0x40;
    ram[0x6F82] = buttons;
    s_frame_completed = true;
}

BOOL system_io_state_write(char* filename, _u8* buffer, _u32 bufferLength) {
    FIL f;
    if (f_open(&f, filename, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return FALSE;
    }
    UINT bw = 0;
    FRESULT fr = f_write(&f, buffer, bufferLength, &bw);
    f_close(&f);
    return (fr == FR_OK && bw == bufferLength) ? TRUE : FALSE;
}

BOOL system_io_state_read(char* filename, _u8* buffer, _u32 bufferLength) {
    FIL f;
    if (f_open(&f, filename, FA_READ) != FR_OK) {
        return FALSE;
    }
    UINT br = 0;
    FRESULT fr = f_read(&f, buffer, bufferLength, &br);
    f_close(&f);
    return (fr == FR_OK && br == bufferLength) ? TRUE : FALSE;
}

static FIL s_state_stream_file;

BOOL system_io_state_stream_open_write(char* filename) {
    ensure_save_directories();
    return (f_open(&s_state_stream_file, filename, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) ? TRUE : FALSE;
}

BOOL system_io_state_stream_open_read(char* filename) {
    return (f_open(&s_state_stream_file, filename, FA_READ) == FR_OK) ? TRUE : FALSE;
}

BOOL system_io_state_stream_write(const _u8* buffer, _u32 bufferLength) {
    UINT bw = 0;
    FRESULT fr = f_write(&s_state_stream_file, buffer, bufferLength, &bw);
    return (fr == FR_OK && bw == bufferLength) ? TRUE : FALSE;
}

BOOL system_io_state_stream_seek(_u32 offset) {
    return (f_lseek(&s_state_stream_file, (FSIZE_t)offset) == FR_OK) ? TRUE : FALSE;
}

BOOL system_io_state_stream_read(_u8* buffer, _u32 bufferLength) {
    UINT br = 0;
    FRESULT fr = f_read(&s_state_stream_file, buffer, bufferLength, &br);
    return (fr == FR_OK && br == bufferLength) ? TRUE : FALSE;
}

void system_io_state_stream_close(void) {
    f_close(&s_state_stream_file);
}

BOOL system_io_flash_write(_u8* buffer, _u32 bufferLength) {
    ensure_save_directories();
    char path[FF_MAX_LFN];
    build_save_path(selectedRom, path, sizeof(path), false);
    FIL f;
    FRESULT fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        build_save_path(selectedRom, path, sizeof(path), true);
        fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    }
    if (fr != FR_OK) {
        return FALSE;
    }
    UINT bw = 0;
    fr = f_write(&f, buffer, bufferLength, &bw);
    f_close(&f);
    return (fr == FR_OK && bw == bufferLength) ? TRUE : FALSE;
}

BOOL system_io_flash_read(_u8* buffer, _u32 bufferLength) {
    char path[FF_MAX_LFN];
    build_save_path(selectedRom, path, sizeof(path), false);
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        build_save_path(selectedRom, path, sizeof(path), true);
        fr = f_open(&f, path, FA_READ);
    }
    if (fr != FR_OK) {
        build_save_path_raw(selectedRom, path, sizeof(path), false);
        fr = f_open(&f, path, FA_READ);
    }
    if (fr != FR_OK) {
        build_save_path_raw(selectedRom, path, sizeof(path), true);
        fr = f_open(&f, path, FA_READ);
    }
    if (fr != FR_OK) {
        return FALSE;
    }
    UINT br = 0;
    fr = f_read(&f, buffer, bufferLength, &br);
    f_close(&f);
    return (fr == FR_OK && br == bufferLength) ? TRUE : FALSE;
}

void system_message(char *vaMessage, ...) {}
void system_sound_chipreset(void) {
    sound_init(AUDIO_SAMPLE_RATE);
}
void system_sound_silence(void) {
    sound_init(AUDIO_SAMPLE_RATE);
}
BOOL system_comms_read(_u8* buffer) { return false; }
BOOL system_comms_poll(_u8* buffer) { return false; }
void system_comms_write(_u8 data) {}

typedef struct {
    char label[9];
    char string[256];
} STRING_TAG;

static STRING_TAG string_tags[] = {
    { "SDEFAULT", "Are you sure you want to revert to the default control setup?" },
    { "ROMFILT",  "Rom Files (*.ngp,*.ngc,*.npc,*.bin)\0*.ngp;*.ngc;*.npc;*.bin\0\0" },
    { "STAFILT",  "State Files (*.ss)\0*.ss\0\0" },
    { "FLAFILT",  "Flash Memory Files (*.sav)\0*.sav\0\0" },
    { "BADFLASH", "Flash data error." },
    { "POWER",    "Power down." },
    { "BADSTATE", "State is from an unsupported version." },
    { "ERROR1",   "Error 1" },
    { "ERROR2",   "Error 2" },
    { "ERROR3",   "Error 3" },
    { "TIMER",    "No high resolution timer." },
    { "WRONGROM", "This state is from a different rom." },
    { "EROMFIND", "Cannot find ROM file" },
    { "EROMOPEN", "Cannot open ROM file" },
    { "EZIPNONE", "No roms found" },
    { "EZIPBAD",  "Corrupted ZIP file" },
    { "EZIPFIND", "Cannot find ZIP file" },
    { "ABORT",    "Abort" },
    { "DISCON",   "Disconnect" },
    { "CONNEC",   "Connected" }
};

char *system_get_string(STRINGS string_id) {
    if (string_id >= STRINGS_MAX)
        return (char *)"Unknown String";
    return string_tags[string_id].string;
}

} // extern "C"

/* =========================================================================
 * Gameplay Session Loop
 * ========================================================================= */
static void run_game(void) {
    apply_safe_qmi_timing();
    const char *ext = strrchr(selectedRom, '.');
    if (ext && (strcasecmp(ext, ".ngc") == 0 || strcasecmp(ext, ".ngpc") == 0)) {
        system_colour = COLOURMODE_COLOUR;
    } else {
        system_colour = COLOURMODE_AUTO;
    }
    language_english = true;
    mute = false;
    sound_init(AUDIO_SAMPLE_RATE);
    bios_install();

    /* The console build holds the ROM in PSRAM (s_rom_ptr); the HW15 build
     * programs it into the flash XIP window. Same bytes, same contract: the
     * core addresses rom.data from ROM_START either way. */
    rom.data = s_rom_in_psram ? s_rom_ptr : (uint8_t *)NGP_ROM_CACHE_ABS;
    rom.length = last_flashed_rom_size;
    rom_loaded();
    reset();

    dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */

    ili9341_clear_black();
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_grid_effect(persistent_grid_effect);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_set_interleave(persistent_interleave);
    ili9341_set_fast_forward(persistent_turbo_mode != 0u);
    ili9341_set_fps_overlay(persistent_display_fps, "60");
#if NGP_PROFILE
    ili9341_prof_enable(true);
#endif
    ili9341_blitter_start();

    s_request_pause_menu = false;
    s_request_exit_game = false;
    reset_requested = false;
    /* Drop any half-finished SELECT+START hold or closed edge latch left over
     * from the previous game. */
    dmg_combo_reset();

    uint32_t frame_start_us = time_us_32();
    uint32_t fps_timer_us = time_us_32();
    uint32_t flash_save_timer_us = 0;
    uint32_t frame_count = 0;
#if NGP_PROFILE
    uint32_t last_frame_us = 16680u;
    uint32_t emu_acc_us = 0;
    uint32_t frame_acc_us = 0;
    ngp_prof_reset();
#endif

    watchdog_enable(6000, false);

    while (!s_request_exit_game) {
        watchdog_update();

        /* Check if in-game flash save occurred and commit with 500ms debounce */
        if (flash_is_dirty()) {
            if (flash_save_timer_us == 0) {
                flash_save_timer_us = time_us_32();
            } else if (time_us_32() - flash_save_timer_us >= 500000u) {
                flash_commit();
                flash_save_timer_us = 0;
            }
        } else {
            flash_save_timer_us = 0;
        }

        if (reset_requested) {
            if (flash_is_dirty()) {
                flash_commit();
                flash_save_timer_us = 0;
            }
            reset();
            reset_requested = false;
        }

        update_ngp_input();

        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (s_request_power_off) {
            s_request_power_off = false;
            ili9341_blitter_stop();
            dmg_power_off_with_resume(&s_ngp_desc, (dmg_power_reason_t)s_power_reason);
        }

        if (s_request_pause_menu) {
            s_request_pause_menu = false;
            if (flash_is_dirty()) {
                flash_commit();
                flash_save_timer_us = 0;
            }
            ingame_action_e action = show_ingame_menu(selectedRom);
            if (action == INGAME_ACTION_EXIT) {
                s_request_exit_game = true;
                break;
            } else if (action == INGAME_ACTION_RESET) {
                if (flash_is_dirty()) {
                    flash_commit();
                    flash_save_timer_us = 0;
                }
                reset();
                reset_requested = false;
            }
            ili9341_clear_black();
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_grid_effect(persistent_grid_effect);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_set_interleave(persistent_interleave);
            ili9341_set_fast_forward(persistent_turbo_mode != 0u);
            ili9341_blitter_start();
            watchdog_update();
            frame_start_us = time_us_32();
            fps_timer_us = time_us_32();
            frame_count = 0;
        }

        system_frameskip_key = persistent_turbo_mode ? 2 : 1;

        /* Emulate until VBL */
        s_frame_completed = false;
#if NGP_PROFILE
        /* Wall-clock cost of the emulated CPU phase alone. Two timer reads a
         * frame, so it is free, and it splits the frame budget between "the
         * interpreter" and "everything around it" -- the split that decides
         * where the next optimisation has to go. */
        const uint32_t emu_t0 = time_us_32();
        while (!s_frame_completed && !s_request_pause_menu && !s_request_exit_game) {
            emulate();
        }
        const uint32_t emu_us = time_us_32() - emu_t0;
#else
        while (!s_frame_completed && !s_request_pause_menu && !s_request_exit_game) {
            emulate();
        }
#endif

        update_audio_frame();
        frame_count++;

        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            ili9341_blitter_stop();
            const bool resumed = ngp_load_state(selectedRom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_clear_black();
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_grid_effect(persistent_grid_effect);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_set_interleave(persistent_interleave);
            ili9341_set_fast_forward(persistent_turbo_mode != 0u);
            ili9341_blitter_start();
            watchdog_update();
            frame_start_us = time_us_32();
            fps_timer_us = time_us_32();
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }
#if NGP_PROFILE
        emu_acc_us += emu_us;
        frame_acc_us += last_frame_us;
#endif

        /* FPS calculation */
        uint32_t now = time_us_32();
        if (now - fps_timer_us >= 1000000u) {
            fps = frame_count;
#if NGP_PROFILE
            /* Per-second averages, not a single frame: one frame is noise. */
            const uint32_t nf = frame_count ? frame_count : 1u;
            const uint32_t frame_avg = frame_acc_us / nf;
            const uint32_t emu_avg = emu_acc_us / nf;
            ili9341_prof_feed(fps, frame_avg, emu_avg);
            emu_acc_us = 0;
            frame_acc_us = 0;
            ngp_prof_reset();
#endif
            frame_count = 0;
            fps_timer_us = now;
            snprintf(fpsString, sizeof(fpsString), "%lu", (unsigned long)fps);
            if (persistent_display_fps) {
                ili9341_set_fps_overlay(true, fpsString);
            }
        }

        /* Frame Pacing (59.95 Hz for NeoGeo Pocket ~16680 us) */
        uint32_t frame_budget = persistent_turbo_mode ? 8340u : 16680u;
        uint32_t target_next_frame = frame_start_us + frame_budget;
        uint32_t now_us = time_us_32();
#if NGP_PROFILE
        last_frame_us = now_us - frame_start_us;
#endif
        if ((int32_t)(target_next_frame - now_us) > 0) {
            busy_wait_us_32(target_next_frame - now_us);
            frame_start_us = target_next_frame;
        } else {
            frame_start_us = now_us;
        }
    }

    watchdog_disable();
    ili9341_blitter_stop();
    flash_commit();
    ili9341_clear_black();
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */
int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    set_sys_clock_khz(CPUFreqKHz, true);
    apply_safe_qmi_timing();

    /* Decouple SPI peripheral clock from CPU:
     * Reconfigure PLL_USB to 140 MHz (VCO = 12 MHz * 70 = 840 MHz, postdiv = 6 * 1 = 6 -> 140 MHz).
     * Route clk_peri to PLL_USB so SPI divisor 2 delivers exactly 70.0 MHz
     * independently of CPU system clock. */
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

    selectedRom[0] = '\0';

    FrensSettings::initSettings(FrensSettings::emulators::MULTI);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, 1024, false, true);
    Frens::blinkLed(false);

#if USE_ILI9341
    ili9341_init();
    dmg_init_boot_mode();
    load_emulator_config();
    set_backlight_brightness(persistent_brightness_level);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_grid_effect(persistent_grid_effect);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_set_interleave(persistent_interleave);
    ili9341_set_fast_forward(persistent_turbo_mode != 0u);
    ili9341_clear_black();
#endif

    ensure_save_directories();

    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_ngp_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_ngp_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_ngp_desc);
            snprintf(selectedRom, sizeof(selectedRom), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_ngp_desc)) dmg_boot_intro_finish();
        }
    }
    if (s_resume_pending) {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dmg_get_current_rom_dir());
    }

    printf("\n==================================================\n");
    printf("  %s NeoGeo Pocket / Color for RP2350\n", APP_TITLE);
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
                ili9341_set_grid_effect(persistent_grid_effect);
                ili9341_set_shimmerless(persistent_shimmerless_enabled);
                ili9341_set_interleave(persistent_interleave);
                ili9341_set_fast_forward(persistent_turbo_mode != 0u);
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

    return 0;
}
