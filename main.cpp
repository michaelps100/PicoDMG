/**
 * PicoA26 for Raspberry Pi RP2350 + Display ILI9341 + DAC I2S MAX98357A
 *
 * Atari 2600 Emulator with Clean Light UI, In-Game Pause Menu,
 * Save/Load States, CRT Scanlines Effect, 60 FPS Dual-Core Display Pipeline,
 * and SD Card Storage.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/pwm.h"

#include "ff.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "external_audio.h"
#include "ili9341.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"

/* Atari emulator core headers */
extern "C" {
#include "a26_core.h"
#include "atari2600emulator.h"
#include "atari/atari_audio.h"
#include "atari_state.h"
}

/* Definition of PalTable for ili9341 driver compatibility */
uint16_t PalTable[64] = {0};

/* =========================================================================
 * Build Target Configuration
 * ========================================================================= */
#define APP_TITLE            "PicoA26"
#define CONFIG_MAGIC         0x41323630u /* "A260" */
#define CONFIG_PATH          "0:/saves/a26/picoa26.cfg"
#define CONFIG_PATH_FALLBACK "/saves/a26/picoa26.cfg"
#define SAVES_DIR            "0:/saves/a26"
#define SAVES_DIR_FALLBACK   "/saves/a26"
#define ROMS_DIR             "0:/roms/a26"
#define ROMS_DIR_FALLBACK    "/roms/a26"
#define ROMS_DIR_ALT         "0:/roms/atari2600"

#if PICO_RP2350
#define EMULATOR_CLOCKFREQ_KHZ 280000
#else
#define EMULATOR_CLOCKFREQ_KHZ 252000
#endif
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

static bool isFatalError = false;
static char selectedRom[FF_MAX_LFN];
static FIL  s_io_fil;

/* =========================================================================
 * Persistent Configuration Structure
 * ========================================================================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t scale_mode;
    uint32_t effect_enabled;
    uint32_t display_fps;
    uint32_t volume_level;
    uint32_t brightness_level;
    uint32_t turbo_mode;
    uint32_t save_slot;
    uint32_t load_slot;
    char     last_rom[FF_MAX_LFN];
} emulator_config_t;

static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_scanlines_enabled = false;
static bool persistent_shimmerless_enabled = true;
static bool persistent_display_fps = false;
static char fpsString[8] = "60";   /* measured, updated once per second */
static uint32_t measured_fps = 60;
static uint8_t persistent_volume_level = 3;     /* 1..5 */
static uint8_t persistent_brightness_level = 5; /* 1..5 */
static uint8_t persistent_turbo_mode = 0;       /* 0: 1x, 1: 2x */
static uint8_t persistent_save_slot = 1;        /* 1..4 */
static uint8_t persistent_load_slot = 1;        /* 1..4 */

/* Static buffers to prevent stack overflow */
static char current_rom_dir[64] = ROMS_DIR;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;
static char s_full_path[FF_MAX_LFN + 32];

#define ROM_UI_BG_COLOR DMG_RGB565_BG_COLOR

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

    if (level >= 5) {
        gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_SIO);
        gpio_set_dir(ILI9341_PIN_BL, GPIO_OUT);
        gpio_put(ILI9341_PIN_BL, 1);
        return;
    }

    gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(ILI9341_PIN_BL);
    uint channel = pwm_gpio_to_channel(ILI9341_PIN_BL);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 4.0f);
    pwm_config_set_wrap(&cfg, 1000);
    pwm_init(slice_num, &cfg, true);

    static const uint16_t duty_table[6] = { 0, 200, 400, 600, 800, 1000 };
    pwm_set_chan_level(slice_num, channel, duty_table[level]);
}

static void ensure_save_directories(void) {
    f_mkdir("0:/saves");
    f_mkdir(SAVES_DIR);
    f_mkdir("/saves");
    f_mkdir(SAVES_DIR_FALLBACK);
}

extern const dmg_emulator_desc_t s_a26_desc;
#define save_emulator_config() a26_save_config()
#define load_emulator_config() a26_load_config()

/* =========================================================================
 * Clean Light UI via shared_menu
 * ========================================================================= */

typedef dmg_action_t ingame_action_t;
#define INGAME_RESUME DMG_ACTION_RESUME
#define INGAME_RESET  DMG_ACTION_RESET
#define INGAME_EXIT   DMG_ACTION_EXIT

static const char *s_a26_candidate_dirs[] = {
    "0:/roms/a26",
    "/roms/a26",
    "roms/a26",
    "0:/roms/atari2600",
    "/roms/atari2600",
    "roms/atari2600",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};

static bool is_atari_rom(const char *filename) {
    if (!filename) return false;
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".a26") == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".rom") == 0);
}

static bool a26_save_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    ensure_save_directories();
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));
    return (atari_save_state(path) == 0);
}

static bool a26_load_state(const char *game_name, uint8_t slot) {
    char path[FF_MAX_LFN];
    dmg_build_state_path(SAVES_DIR, game_name, slot, path, sizeof(path));
    return (atari_load_state(path) == 0);
}

static void a26_apply_scale(dmg_scale_mode_t mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void a26_apply_effect(bool effect) {
    persistent_scanlines_enabled = effect;
    ili9341_set_scanlines(persistent_scanlines_enabled);
}

static void a26_apply_filter(bool filter) {
    persistent_shimmerless_enabled = filter;
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

static void a26_apply_speed(bool turbo) {
    persistent_turbo_mode = turbo ? 1 : 0;
}

static void a26_apply_volume(uint8_t vol) {
    if (vol > 5) vol = 5;
    persistent_volume_level = vol;
}

static void a26_apply_brightness(uint8_t br) {
    set_backlight_brightness(br);
}

/* Combo hooks, defined after the descriptor they are stored in. */
static void a26_save_config(void);
static bool a26_get_fps(void);
static void a26_apply_fps(bool on);

static bool a26_on_load_rom(const struct dmg_emulator_desc *desc, const char *filename) {
    (void)desc;
    (void)filename;
    const char *dir = dmg_get_current_rom_dir();
    if (dir && dir[0]) {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dir);
    }
    return true;
}

static void draw_a26_image_row(uint8_t idx, bool selected, ili9341_scale_mode_t scale, bool scanlines, bool shimmerless) {
    if (idx >= 3) return;
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? DMG_RGB565_SEL_COLOR : DMG_RGB565_BG_COLOR;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    if (dmg_is_gb_mode()) {
        if (wx > 0) ili9341_fill_rect(0, y, wx, DMG_UI_ITEM_HEIGHT, 0x0000);
        uint16_t rx = (uint16_t)(wx + ww);
        if (rx < 320) ili9341_fill_rect(rx, y, (uint16_t)(320 - rx), DMG_UI_ITEM_HEIGHT, 0x0000);
    }

    ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    char buf[44];
    buf[0] = '\0';

    if (idx == 0) {
        const char *scale_names[] = { "< Fit >", "< Stretch >", "< Integer >" };
        snprintf(buf, sizeof(buf), "Scale:        %s", scale_names[scale]);
    } else if (idx == 1) {
        snprintf(buf, sizeof(buf), "Effect:       %s", scanlines ? "< Scanlines >" : "< None >");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Filter:       %s", shimmerless ? "< Shimmerless >" : "< Sharp >");
    }

    ili9341_text(buf, (uint16_t)(wx + 12u), (uint16_t)(y + 7u), selected ? DMG_RGB565_TEXT_SEL_COLOR : DMG_RGB565_TEXT_COLOR, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void a26_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    ili9341_scale_mode_t sel_scale = persistent_scale_mode;
    bool sel_scanlines = persistent_scanlines_enabled;
    bool sel_shimmerless = persistent_shimmerless_enabled;

    dmg_wait_for_all_buttons_released();

    ili9341_fill_screen(0x0000);
    if (!dmg_is_gb_mode()) {
        ili9341_fill_screen(DMG_RGB565_BG_COLOR);
    } else {
        ili9341_fill_rect(wx, 0, ww, 240, DMG_RGB565_BG_COLOR);
    }
    dmg_draw_top_bar(desc, APP_TITLE, "Image Settings");
    dmg_draw_game_bar(desc, game_name);
    dmg_draw_bottom_bar(desc, "L/R: Adjust", "B: Apply & Back");

    for (uint8_t i = 0; i < 3; i++) {
        draw_a26_image_row(i, i == cursor, sel_scale, sel_scanlines, sel_shimmerless);
    }
    for (uint8_t i = 3; i < 8; i++) {
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
            cursor = (cursor == 0) ? 2 : (cursor - 1);
            draw_a26_image_row(prev, false, sel_scale, sel_scanlines, sel_shimmerless);
            draw_a26_image_row(cursor, true, sel_scale, sel_scanlines, sel_shimmerless);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == 2) ? 0 : (cursor + 1);
            draw_a26_image_row(prev, false, sel_scale, sel_scanlines, sel_shimmerless);
            draw_a26_image_row(cursor, true, sel_scale, sel_scanlines, sel_shimmerless);
        }

        if (pushed_left || pushed_right || pushed_a) {
            if (cursor == 0) {
                if (pushed_right || pushed_a) {
                    sel_scale = (ili9341_scale_mode_t)((sel_scale + 1) % 3);
                } else {
                    sel_scale = (ili9341_scale_mode_t)((sel_scale == 0) ? 2 : (sel_scale - 1));
                }
                draw_a26_image_row(0, true, sel_scale, sel_scanlines, sel_shimmerless);
            } else if (cursor == 1) {
                sel_scanlines = !sel_scanlines;
                draw_a26_image_row(1, true, sel_scale, sel_scanlines, sel_shimmerless);
            } else if (cursor == 2) {
                sel_shimmerless = !sel_shimmerless;
                draw_a26_image_row(2, true, sel_scale, sel_scanlines, sel_shimmerless);
            }
        }

        if (pushed_b) {
            persistent_scale_mode = sel_scale;
            persistent_scanlines_enabled = sel_scanlines;
            persistent_shimmerless_enabled = sel_shimmerless;
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            dmg_config_save(desc);
            dmg_wait_for_all_buttons_released();
            return;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}

#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
/* Resume intro chime, pushed from core 1 into the I2S ring (dmg_boot_intro.c). */
static int  a26_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void a26_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void a26_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(44100);
}
#endif

const dmg_emulator_desc_t s_a26_desc = {
    .app_title            = APP_TITLE,
    .default_rom_dir      = ROMS_DIR,
    .candidate_dirs       = s_a26_candidate_dirs,
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
    .is_rom_file          = is_atari_rom,
    .on_load_rom          = a26_on_load_rom,
    .save_state           = a26_save_state,
    .load_state           = a26_load_state,
    .apply_scale          = a26_apply_scale,
    .apply_effect         = a26_apply_effect,
    .apply_interleave     = NULL,
    .apply_speed          = a26_apply_speed,
    .apply_volume         = a26_apply_volume,
    .apply_brightness     = a26_apply_brightness,
    .apply_filter         = a26_apply_filter,
    .reset_game           = NULL,
    .exit_game            = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu       = a26_image_submenu,
    .save_config          = a26_save_config,
    .get_fps              = a26_get_fps,
    .apply_fps            = a26_apply_fps,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_free           = a26_audio_free,
    .audio_push           = a26_audio_push,
#endif
    .flush_saves          = NULL,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_restore        = a26_audio_restore
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
static void a26_save_config(void) {
    g_dmg_config.scale_mode       = (uint32_t)persistent_scale_mode;
    g_dmg_config.grid_effect      = persistent_scanlines_enabled ? 1u : 0u;
    g_dmg_config.turbo_mode       = persistent_turbo_mode;
    g_dmg_config.volume_level     = persistent_volume_level;
    g_dmg_config.brightness_level = persistent_brightness_level;
    g_dmg_config.save_slot        = persistent_save_slot;
    g_dmg_config.load_slot        = persistent_load_slot;
    /* dmg_config_t has no fps field, so it rides in the spare extra_opt1. */
    g_dmg_config.extra_opt1       = persistent_display_fps ? 1u : 0u;
    g_dmg_config.extra_opt2       = persistent_shimmerless_enabled ? 0u : 1u;
    dmg_config_save(&s_a26_desc);
}

static void a26_load_config(void) {
    /* dmg_config_load() pushes scale/effect/interleave/speed/volume/brightness
     * back into the persistent_* variables through the descriptor callbacks.
     * extra_opt1 has no callback of its own, so the FPS overlay is read and
     * applied by hand. */
    dmg_config_load(&s_a26_desc);
    persistent_display_fps = (g_dmg_config.extra_opt1 != 0u);
    if (s_a26_desc.apply_fps != NULL) s_a26_desc.apply_fps(persistent_display_fps);
}


static bool a26_get_fps(void) {
    return persistent_display_fps;
}

static void a26_apply_fps(bool on) {
    persistent_display_fps = on;
    ili9341_set_fps_overlay(persistent_display_fps, fpsString);
}

static bool rom_file_selector(char *selected_out, size_t max_len) {
    return dmg_rom_selector(&s_a26_desc, selected_out, max_len);
}

static ingame_action_t ingame_menu(const char *game_name) {
    return dmg_ingame_menu(&s_a26_desc, game_name);
}


/* =========================================================================
 * Cartridge ROM Loading from SD Card
 * ========================================================================= */
static bool load_cart_rom_file(const char *filename) {
    if (strchr(filename, '/') != NULL || current_rom_dir[0] == '\0') {
        snprintf(s_full_path, sizeof(s_full_path), "%s", filename);
    } else {
        snprintf(s_full_path, sizeof(s_full_path), "%s/%s", current_rom_dir, filename);
    }

    printf("[PicoA26] Loading ROM: %s\n", s_full_path);

    ili9341_blitter_stop();
    if (!dmg_boot_intro_active()) {
        ili9341_fill_screen(0x0000);
        ili9341_text("Carregando...", 20, 80, 0xFFFF, 0x0000);
        ili9341_text(filename, 20, 105, 0x07E0, 0x0000);
        ili9341_flush(true);
    }

    size_t max_cart_sz = 0;
    uint8_t *cart_buf = a26_core_get_rom_buffer(&max_cart_sz);
    if (!cart_buf || max_cart_sz == 0) {
        printf("[PicoA26] Cartridge RAM buffer unavailable!\n");
        return false;
    }

    FIL fil;
    FRESULT fr = f_open(&fil, s_full_path, FA_READ);
    if (fr != FR_OK) {
        printf("[PicoA26] f_open error: %d\n", fr);
        return false;
    }

    /* Turn LED ON strictly during SD read activity into RAM */
    Frens::blinkLed(true);
    FSIZE_t file_size=f_size(&fil);
    size_t header=0;
    if(file_size%1024==128) header=128;
    else if(file_size%1024==64) header=64;
    else if(file_size%1024==32) header=32;
    if(file_size<=header || file_size-header>max_cart_sz) {
        f_close(&fil); Frens::blinkLed(false);
        if (!dmg_boot_intro_active()) {
            ili9341_text("ROM invalida ou maior que 64 KB",10,140,0xffff,0);
            ili9341_flush(true);
        }
        return false;
    }
    if(header && f_lseek(&fil,header)!=FR_OK) {
        f_close(&fil); Frens::blinkLed(false); return false;
    }
    UINT br = 0;
    fr = f_read(&fil, cart_buf, (UINT)(file_size-header), &br);
    f_close(&fil);
    /* Turn LED OFF immediately after read finishes */
    Frens::blinkLed(false);

    if (fr != FR_OK || br != file_size-header) {
        printf("[PicoA26] f_read error: %d\n", fr);
        return false;
    }

    printf("[PicoA26] ROM loaded directly into SRAM (%u bytes)\n", (unsigned)br);
    if (!a26_core_load_rom(cart_buf, br)) {
        printf("[PicoA26] %s\n",a26_core_last_error());
        if (!dmg_boot_intro_active()) {
            ili9341_text(a26_core_last_error(),10,140,0xffff,0);
            ili9341_flush(true);
        }
        return false;
    }
    a26_core_set_region_hint(filename);
    return true;
}

static void __not_in_flash_func(a26_line_render_cb)(int line, const uint16_t *pixels) {
    ili9341_stream_a26_line(line, pixels);
}

/* Called by the core at 44.1 kHz of emulated time; the TIA itself runs at
 * colour_clock/114 inside a26_core_run_frame(). */
static void __not_in_flash_func(a26_audio_step_cb)(void) {
    int16_t sample=atari_audio_get_sample(persistent_volume_level);
    EXT_AUDIO_ENQUEUE_SAMPLE(sample,sample);
}
static void reset_audio_output(void) {
#if USE_I2S_AUDIO && !USE_PICO_EXTRAS_I2S
    audio_i2s_flush();
#endif
}

/* =========================================================================
 * Main Gameplay Loop
 * ========================================================================= */
static void run_game(void) {
    Frens::blinkLed(false);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_fill_screen(0x0000);
    ili9341_blitter_start();

    /* Fixed-deadline schedule for frame pacing; see the comment at the pacing
     * site below for why a sleep-the-remainder loop was not enough. */
    uint32_t next_frame_deadline = time_us_32();
    uint32_t frame_counter = 0, presented_counter=0;
    uint64_t pacing_fraction=0;
    reset_audio_output();
    uint32_t fps_timer = time_us_32();

    bool in_game = true;

    /* Drop any half-finished SELECT+START hold or closed edge latch left over
     * from the previous game. */
    dmg_combo_reset();
    
    /* Enable the watchdog for the gameplay loop.
     *
     * It was never enabled anywhere in the PicoA26 runtime (only inside the
     * flash-write helper and the unused legacy FruitJam launcher), so every
     * hang was permanent: no reset, no recovery, a frozen last frame forever.
     * The feed points are a26_core_run_frame()'s scanline branch and the two
     * blitter waits in ili9341.c -- the only unbounded loops in the loop body.
     *
     * 6 s is the SDK default and deliberately generous: it must not fire during
     * a legitimate pause (the pause menu is driven by the shared menu, which
     * feeds it) or during an SD write. Its job is only to break the
     * core0/core1 deadlock class and a runaway game loop. */
    watchdog_enable(6000, false);

    while (in_game) {
        watchdog_update();
        /* Read buttons */
        dmg_pad_t pad_in;
        read_ui_buttons(&pad_in.up, &pad_in.down, &pad_in.left, &pad_in.right,
                        &pad_in.a, &pad_in.b, &pad_in.select, &pad_in.start);

        /* Combos: SELECT+START exits (held ~1 s), START+B pauses, START+A toggles
         * the FPS overlay, SELECT+A toggles 1x/2x, SELECT+UP/DOWN is the volume.
         * dmg_combo_frame() masks the buttons a combo consumed, so the pad below
         * is already clean by the time the core sees it. */
        dmg_combo_events_t combo;
        dmg_combo_frame(&pad_in, &s_a26_desc, &combo);

        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (combo.request_power_off) {
            ili9341_blitter_stop();
            dmg_power_off_with_resume(&s_a26_desc, (dmg_power_reason_t)combo.power_reason);
        }

        if (combo.request_pause_menu) {
            /* Open In-Game Pause Menu */
            ili9341_blitter_stop();
            ingame_action_t act = ingame_menu(selectedRom);
            if (act == INGAME_EXIT) {
                in_game = false;
                break;
            } else if (act == INGAME_RESET) {
                a26_core_reset();
            }
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_blitter_start();
            reset_audio_output();
            next_frame_deadline=time_us_32(); pacing_fraction=0;
            /* Re-stamp the overlay: blitter_start() re-arms the frame window and
             * the menu drew over the top border while it was up. */
            if (persistent_display_fps) {
                ili9341_set_fps_overlay(true, fpsString);
            }
        }

        if (combo.request_exit_game) {
            /* Exit game back to ROM selector */
            in_game = false;
            break;
        }

        bool btn_a = pad_in.a, btn_b = pad_in.b;
        bool btn_select = pad_in.select, btn_start = pad_in.start;

        /* Update Atari 2600 Controls */
        a26_core_set_controls(pad_in.up, pad_in.down, pad_in.left, pad_in.right,
                             (btn_a || btn_b), btn_select, btn_start);

        // Draw only when the previous presentation has finished. The core
        // still renders collisions, clocks audio and samples controls every frame.
        bool present=!ili9341_is_frame_busy();
        if(!ili9341_blitter_ok()) break;
        uint32_t frame_color_hz=a26_core_color_clock_hz();
        a26_frame_result_t frame=a26_core_run_frame(present?a26_line_render_cb:nullptr,a26_audio_step_cb);
        if(frame.reason==A26_FRAME_JAM) {
            printf("[A26] CPU JAM em PC=%04x; retornando ao seletor\n",cpu.pc);
            break;
        }
        if(present && ili9341_try_submit_a26_frame()) ++presented_counter;
        ++frame_counter;

        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            ili9341_blitter_stop();
            const bool resumed = a26_load_state(selectedRom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_blitter_start();
            reset_audio_output();
            next_frame_deadline = time_us_32(); pacing_fraction = 0;
            if (persistent_display_fps) ili9341_set_fps_overlay(true, fpsString);
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }
        uint32_t speed=persistent_turbo_mode?2u:1u;
        pacing_fraction+=(uint64_t)frame.color_clocks*1000000u;
        uint32_t target_us=(uint32_t)(pacing_fraction/(frame_color_hz*speed));
        pacing_fraction%=frame_color_hz*speed;
        next_frame_deadline+=target_us;
        int32_t remaining=(int32_t)(next_frame_deadline-time_us_32());
        if(remaining>0) sleep_us((uint32_t)remaining);
        else if(remaining< -100000) next_frame_deadline=time_us_32();

        if (time_us_32() - fps_timer >= 1000000) {
            const uint32_t now = time_us_32();
            measured_fps = (uint32_t)((uint64_t)frame_counter*1000000u/(now-fps_timer));
            presented_counter=0;
            snprintf(fpsString, sizeof(fpsString), "%lu", (unsigned long)measured_fps);
            fps_timer = now;
            frame_counter = 0;

            /* The overlay sits in screen rows 8..15, above the blitter's window
             * (24..215), so the game never overwrites it and it does not need
             * to be restamped per frame. One small write per second is enough
             * to keep the number live, at a cost the NGP's per-frame stamp does
             * not have. */
            if (persistent_display_fps) {
                ili9341_set_fps_overlay(true, fpsString);
            }
        }
    }

    ili9341_blitter_stop();
    reset_audio_output();
    wait_for_all_ui_buttons_released();
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */
int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10); // Let VREG settle before the first overclock (Frens repeats this later).
    set_sys_clock_khz(CPUFreqKHz, true);

    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
        clock_get_hz(clk_sys),
        clock_get_hz(clk_sys)
    );

    stdio_init_all();
    init_ui_buttons();

    selectedRom[0] = '\0';

    FrensSettings::initSettings(FrensSettings::emulators::MULTI);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, 1024, false, true);
    Frens::blinkLed(false);

    ili9341_init();
    dmg_init_boot_mode();
    load_emulator_config();
    set_backlight_brightness(persistent_brightness_level);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    if (dmg_is_gb_mode()) {
        ili9341_fill_screen(0x0000);
        ili9341_fill_rect(dmg_get_window_x(), 0, dmg_get_window_w(), 240, ROM_UI_BG_COLOR);
    } else {
        ili9341_fill_screen(ROM_UI_BG_COLOR);
    }

    ensure_save_directories();

    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_a26_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_a26_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_a26_desc);
            snprintf(selectedRom, sizeof(selectedRom), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_a26_desc)) dmg_boot_intro_finish();
        }
    }
    if (s_resume_pending) {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dmg_get_current_rom_dir());
    }

    printf("\n==================================================\n");
    printf("  %s Atari 2600 Emulator for RP2350\n", APP_TITLE);
    printf("  Build: 1.5-enduro-r5 %s %s\n",__DATE__,__TIME__);
    printf("  CPU: %lu MHz | TFT 320x240, apresentacao assincrona\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    printf("  SRAM Mode: Internal SRAM (520KB, NO PSRAM required)\n");
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
                dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */
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
