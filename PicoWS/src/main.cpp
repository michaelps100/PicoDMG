/**
 * PicoWS — WonderSwan Emulator for RP2350 + ILI9341
 *
 * Waveshare RP2350 Plus 16MB | SPI0: ILI9341 | SPI1: SD Card
 * GPIO pins (HW_CONFIG=15):
 *   ILI9341: CS=17, SCK=18, MOSI=19, DC=20, RST=21, BL=22 (SPI0 → PLL_USB 70MHz)
 *   SD Card: CS=13, SCK=14, MOSI=15, MISO=12 (SPI1)
 *   I2S Audio: DATA=26, CLK=27
 *   LED: GPIO 25
 *
 * ROM layout:
 *   0x10000000 → firmware binary
 *   0x10080000 → ROM cache start (512KB headroom after firmware)
 *   0x10080000 - FLASH_SECTOR_SIZE → cache header
 *
 * SD card:
 *   roms/ws/    — WonderSwan ROMs (.ws, .wsc)
 *   saves/ws/   — Save states (_1.ss .. _4.ss), SRAM (.sav), config (picows.cfg)
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/structs/xip.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/pwm.h"
#include "hardware/pll.h"
#include "hardware/pio.h"

#include "ff.h"
#include "tf_card.h"
#include "ili9341.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#include "FrensHelpers.h"
#include "external_audio.h"

extern "C" {
#include "oswan/ws.h"
#include "oswan/gpu.h"
#include "oswan/memory.h"
#include "oswan/io.h"
#include "oswan/rom.h"
#include "oswan/ws_audio.h"
#include "oswan/ws_snd_trace.h"
}
#include "ws_state_io.h"

/* =========================================================================
 * Build configuration
 * ========================================================================= */
#define APP_TITLE      "PicoWS"
#define CONFIG_MAGIC   0x57535053u   /* "WSPS" */
#define ROMS_DIR              "0:/roms/ws"
#define ROMS_DIR_FALLBACK     "/roms/ws"
#define SAVES_DIR             "0:/saves/ws"
#define SAVES_DIR_FALLBACK    "/saves/ws"
#define CONFIG_PATH           "0:/saves/ws/picows.cfg"
#define CONFIG_PATH_FALLBACK  "/saves/ws/picows.cfg"

#ifndef SDCARD_SPI
#define SDCARD_SPI spi1
#endif
#ifndef SDCARD_PIN_MISO
#define SDCARD_PIN_MISO 12
#endif
#ifndef SDCARD_PIN_CS
#define SDCARD_PIN_CS 13
#endif
#ifndef SDCARD_PIN_SCK
#define SDCARD_PIN_SCK 14
#endif
#ifndef SDCARD_PIN_MOSI
#define SDCARD_PIN_MOSI 15
#endif
#ifndef SDCARD_PIO
#define SDCARD_PIO pio1
#endif

/* Flash ROM cache: 1MB headroom after bootloader offset 0x10080000 */
#define WS_ROM_CACHE_HEADROOM  0x100000u   /* 1 MB headroom beyond flash code */
#define WS_ROM_CACHE_ABS       (0x10080000u + WS_ROM_CACHE_HEADROOM)
#define WS_ROM_CACHE_OFF       (WS_ROM_CACHE_ABS - 0x10000000u)
#define WS_ROM_CACHE_MAX       0x400000u   /* 4 MB max ROM size */
#define WS_CACHE_MAGIC         0x57535043u /* "WSPC" */
#define WS_CACHE_HDR_OFF       (WS_ROM_CACHE_OFF - FLASH_SECTOR_SIZE)
#define WS_CACHE_HDR_ABS       (WS_ROM_CACHE_ABS - FLASH_SECTOR_SIZE)

#if PICO_RP2350
#define CPU_FREQ_KHZ  360000u
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
#define CPU_FREQ_KHZ  252000u
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

/* =========================================================================
 * Cache header (packed into one flash page)
 * ========================================================================= */
typedef struct __attribute__((aligned(4))) {
    uint32_t magic;
    uint32_t rom_size;
    uint32_t sample_crc;
    char     filename[64];
    uint8_t  padding[256 - 76];
} PicoWS_Cache_Header_t;
static_assert(sizeof(PicoWS_Cache_Header_t) == 256, "PicoWS_Cache_Header_t must be exactly 256 bytes");

static uint32_t compute_ws_sample_hash(const uint8_t *base, uint32_t size) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 32; i++) {
        uint32_t off = (size > 512) ? (uint32_t)(((uint64_t)i * (size - 512)) / 31) : 0;
        off &= ~3u;
        const uint32_t *p = (const uint32_t *)(base + off);
        for (int w = 0; w < 128; w++) hash = (hash ^ p[w]) * 16777619u;
    }
    return hash;
}

/* =========================================================================
 * Clean Light UI Common & Emulator Descriptor (shared_menu)
 * ========================================================================= */
static ili9341_scale_mode_t cfg_scale      = SCALE_FIT;
static bool                 cfg_grid       = false;
static bool                 cfg_shimmerless = true;
static bool                 cfg_interleave = false;
static bool                 cfg_turbo      = false;
static bool                 cfg_fps        = false;
static uint8_t              cfg_bright     = 5;
static uint8_t              cfg_volume     = 3;
static uint8_t              cfg_save_slot  = 1;
static uint8_t              cfg_load_slot  = 1;
static uint8_t              cfg_palette    = 0;    /* 0=Default,1=Amber,2=Green */

/* Full frame capture for 90-degree rotated Vertical (Tate) display */
#if defined(PSRAM_CS_PIN) && PSRAM_CS_PIN
/* Tate-mode frame: 31 KB that only matters when the screen is rotated. On the
 * console it lives in PSRAM (allocated on first use) so SRAM can go to the core. */
static uint8_t *s_ws_vframe = nullptr;
#else
static uint8_t  s_ws_vframe[144 * 224];
#endif
static bool     s_ws_vertical = false;

static FATFS    s_fs;
static FIL      s_fil;
static char     s_selected_rom[FF_MAX_LFN];
static uint32_t s_last_flashed_size = 0;

/* Where the running cartridge lives.
 *
 * On the console build (PSRAM_CS_PIN != 0) the ROM is read straight from the SD
 * card into PSRAM and s_rom_ptr points there; the HW15 build has no PSRAM and
 * programs it into the flash XIP window at WS_ROM_CACHE_ABS instead, leaving
 * s_rom_ptr NULL. A flag rather than an address-range test, because a PSRAM
 * pointer is in the 0x11xxxxxx range and the XIP window is a perfectly
 * legal-looking 0x1018.... */
static uint8_t *s_rom_ptr = NULL;
static bool s_rom_in_psram = false;
static char     s_fps_str[40] = "60";
static char     s_full_path[256];

#define init_ui_buttons() dmg_init_ui_buttons()
#define read_ui_buttons(u, d, l, r, a, b, sel, sta) dmg_read_ui_buttons(u, d, l, r, a, b, sel, sta)
#define wait_buttons_released() dmg_wait_for_all_buttons_released()

#define get_basename dmg_sanitize_basename
#define ROM_NAME_LEN 80u
#define get_saves_dir() "0:/saves/ws"

#define UI_BG    DMG_RGB565_BG_COLOR
#define UI_BAR   DMG_RGB565_BAR_COLOR
#define UI_BDR   DMG_RGB565_BORDER_COLOR
#define UI_TXT   DMG_RGB565_TEXT_COLOR
#define UI_TBRT  DMG_RGB565_TEXT_BAR_COLOR
#define UI_SEL   DMG_RGB565_SEL_COLOR
#define UI_OK    DMG_RGB565_STATUS_OK
#define UI_ERR   DMG_RGB565_STATUS_ERR

static void set_sound_volume_level(uint8_t lv) {
    if (lv > 5) lv = 5;
    cfg_volume = lv;
    ws_audio_set_volume(lv);
}

static void set_brightness(uint8_t lv) {
    if (lv < 1) lv = 1;
    if (lv > 5) lv = 5;
    cfg_bright = lv;
    if (lv >= 5) {
        gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_SIO);
        gpio_set_dir(ILI9341_PIN_BL, GPIO_OUT);
        gpio_put(ILI9341_PIN_BL, 1);
    } else {
        gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_PWM);
        uint s = pwm_gpio_to_slice_num(ILI9341_PIN_BL);
        pwm_config c = pwm_get_default_config();
        pwm_config_set_clkdiv(&c, 4.0f);
        pwm_config_set_wrap(&c, 1000);
        pwm_init(s, &c, true);
        static const uint16_t dt[6] = {0, 200, 400, 600, 800, 1000};
        pwm_set_gpio_level(ILI9341_PIN_BL, dt[lv]);
    }
}

static void ensure_save_dirs(void) {
    f_mkdir("0:/saves");
    f_mkdir("0:/saves/ws");
    f_mkdir("/saves");
    f_mkdir("/saves/ws");
    f_mkdir("saves");
    f_mkdir("saves/ws");
    f_mkdir("0:/roms");
    f_mkdir("0:/roms/ws");
    f_mkdir("/roms");
    f_mkdir("/roms/ws");
    f_mkdir("roms");
    f_mkdir("roms/ws");
}

static bool ws_is_rom_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".ws") == 0 ||
            strcasecmp(dot, ".wsc") == 0);
}

static bool ws_save_state(const char *game_name, uint8_t slot) {
    char path[256];
    ensure_save_dirs();
    dmg_build_state_path("0:/saves/ws", game_name, slot, path, sizeof(path));
    return ws_state_save(path);
}

static bool ws_load_state(const char *game_name, uint8_t slot) {
    char path[256];
    dmg_build_state_path("0:/saves/ws", game_name, slot, path, sizeof(path));
    FILINFO fno;
    bool exists = (f_stat(path, &fno) == FR_OK && fno.fsize > 0);
    if (!exists) {
        dmg_build_state_path("/saves/ws", game_name, slot, path, sizeof(path));
        exists = (f_stat(path, &fno) == FR_OK && fno.fsize > 0);
        if (!exists) return false;
    }
    return ws_state_load(path);
}

/* Slot probe for the shared menu's Load State row.
 *
 * ws_load_state() above tries the numbered file in "0:/saves/ws" and then in
 * "/saves/ws", so the probe has to try both. The generic single-path probe would
 * grey out a slot that loads, on a card whose root mounted without the volume
 * prefix. */
static bool ws_state_exists(const char *game_name, uint8_t slot) {
    if (slot < 1 || slot > 4) return false;

    char path[256];
    dmg_build_state_path("0:/saves/ws", game_name, slot, path, sizeof(path));
    if (dmg_state_file_present(path)) return true;

    dmg_build_state_path("/saves/ws", game_name, slot, path, sizeof(path));
    return dmg_state_file_present(path);
}

static void ws_apply_scale(dmg_scale_mode_t scale_mode) {
    cfg_scale = (ili9341_scale_mode_t)scale_mode;
    ili9341_set_scale_mode(cfg_scale);
}

static void ws_apply_effect(bool effect_on) {
    cfg_grid = effect_on;
    ili9341_set_grid_effect(cfg_grid);
}

static void ws_apply_interleave(bool interleave_on) {
    cfg_interleave = interleave_on;
    ili9341_set_interleave(cfg_interleave);
}

static void ws_apply_filter(bool shimmerless_on) {
    cfg_shimmerless = shimmerless_on;
    ili9341_set_shimmerless(cfg_shimmerless);
}

static void ws_apply_speed(bool turbo_on) {
    cfg_turbo = turbo_on;
}

static void ws_apply_volume(uint8_t volume) {
    cfg_volume = volume;
    set_sound_volume_level(volume);
}

static void ws_apply_brightness(uint8_t brightness) {
    cfg_bright = brightness;
    set_brightness(brightness);
}

static void image_submenu(const char *game_name);

/* All three are defined just after the descriptor that stores them, which is
 * why they need forward declarations here. */
static void save_config(void);
static bool ws_get_fps(void);
static void ws_apply_fps(bool on);

/* Battery save (.sav) of the running game. ws_sram_epoch is bumped by the core on every real
 * change to cartridge SRAM / external EEPROM; the .sav is written whenever it differs from
 * the epoch last written, so a save made inside the game reaches the card without having to
 * leave the game first. */
extern "C" volatile uint32_t ws_sram_epoch;
static char     s_sav_path[ROM_NAME_LEN + 32];
static bool     s_sav_active = false;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;
static uint32_t s_sav_epoch  = 0;

static void ws_flush_battery_save(void) {
    if (!s_sav_active) return;
    const uint32_t ep = ws_sram_epoch;
    if (ep == s_sav_epoch) return;
    if (ws_sram_save(s_sav_path)) s_sav_epoch = ep;
    else printf("[E] Battery save failed: %s\n", s_sav_path);
}

/* The power switch parks Core 1 through this callback before standby, and the pause
 * menu before drawing: both are moments the .sav must be on the card. */
static void ws_stop_blitter(void) {
    ili9341_blitter_stop();
    ws_flush_battery_save();
}

static void ws_custom_submenu(const struct dmg_emulator_desc *desc, const char *game_name) {
    (void)desc;
    image_submenu(game_name);
}

static const char *s_ws_candidate_dirs[] = {
    "0:/roms/ws",
    "0:/roms/wonderswan",
    "0:/roms",
    "/roms/ws",
    "/roms",
    NULL
};

#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
/* Resume intro chime, pushed from core 1 into the I2S ring (dmg_boot_intro.c). */
static int  ws_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void ws_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void ws_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(44100);
}
#endif

static const dmg_emulator_desc_t s_ws_desc = {
    .app_title = "PicoWS",
    .default_rom_dir = "0:/roms/ws",
    .candidate_dirs = s_ws_candidate_dirs,
    .config_path = "0:/saves/ws/picows.cfg",
    .config_fallback = "/saves/ws/picows.cfg",
    .saves_dir = "0:/saves/ws",
    .config_magic = 0x57535053u, /* 'WSPS' */
    .is_rgb555 = false,
    .display = {
        .fill_rect = ili9341_fill_rect,
        .draw_text = ili9341_text,
        .flush_rows = ili9341_flush_rows,
        .fill_screen = ili9341_fill_screen,
        .flush = ili9341_flush,
        .stop_blitter = ws_stop_blitter,
        .start_blitter = ili9341_blitter_start,
        .write_rect_565 = ili9341_write_rect_565,
    },
    .is_rom_file = ws_is_rom_file,
    .on_load_rom = NULL,
    .save_state = ws_save_state,
    .load_state = ws_load_state,
    .apply_scale = ws_apply_scale,
    .apply_effect = ws_apply_effect,
    .apply_interleave = ws_apply_interleave,
    .apply_speed = ws_apply_speed,
    .apply_volume = ws_apply_volume,
    .apply_brightness = ws_apply_brightness,
    .apply_filter = ws_apply_filter,
    .reset_game = NULL,
    .exit_game = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu = ws_custom_submenu,
    .save_config = save_config,
    .get_fps = ws_get_fps,
    .apply_fps = ws_apply_fps,
    .state_exists = ws_state_exists,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_free           = ws_audio_free,
    .audio_push           = ws_audio_push,
#endif
    .flush_saves          = ws_flush_battery_save,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_restore        = ws_audio_restore
#endif
};

static bool ws_get_fps(void) {
    return cfg_fps;
}

static void ws_apply_fps(bool on) {
    cfg_fps = on;
    ili9341_set_fps_overlay(cfg_fps, s_fps_str);
}

static void save_config(void) {
    g_dmg_config.scale_mode = (uint32_t)cfg_scale;
    g_dmg_config.grid_effect = cfg_grid ? 1u : 0u;
    g_dmg_config.interleave = cfg_interleave ? 1u : 0u;
    g_dmg_config.turbo_mode = cfg_turbo ? 1u : 0u;
    g_dmg_config.volume_level = cfg_volume;
    g_dmg_config.brightness_level = cfg_bright;
    g_dmg_config.save_slot = cfg_save_slot;
    g_dmg_config.load_slot = cfg_load_slot;
    g_dmg_config.extra_opt1 = cfg_palette;
    g_dmg_config.extra_opt2 = cfg_shimmerless ? 0u : 1u;
    dmg_config_save(&s_ws_desc);
}

static void load_config(void) {
    dmg_config_load(&s_ws_desc);
    cfg_scale = (ili9341_scale_mode_t)g_dmg_config.scale_mode;
    cfg_grid = g_dmg_config.grid_effect != 0;
    cfg_interleave = g_dmg_config.interleave != 0;
    cfg_turbo = g_dmg_config.turbo_mode != 0;
    cfg_volume = (uint8_t)g_dmg_config.volume_level;
    cfg_bright = (uint8_t)g_dmg_config.brightness_level;
    cfg_save_slot = (uint8_t)g_dmg_config.save_slot;
    cfg_load_slot = (uint8_t)g_dmg_config.load_slot;
    cfg_palette = (uint8_t)g_dmg_config.extra_opt1;
    cfg_shimmerless = (g_dmg_config.extra_opt2 != 1u);
}

static bool rom_file_selector(char *out, size_t max) {
    return dmg_rom_selector(&s_ws_desc, out, max);
}

/* Reads the cartridge straight from the SD card into PSRAM.
 *
 * On the console build (PSRAM_CS_PIN != 0) this replaces the flash XIP path, and
 * the reason is visible in the numbers rather than in principle:
 *
 *  - WonderSwan carts go up to 4 MB, and WS_ROM_CACHE_MAX is exactly 4 MB, so
 *    the flash path erases and rewrites megabytes of QSPI on every load. That is
 *    what the persistent cache header, the 32-block spot check and the
 *    safe_flash_range_* wrappers all exist to make bearable, and it is the most
 *    likely reason loading a large cart approaches the 6 s watchdog.
 *  - The HW_CONFIG 15 build sits at 92% SRAM with ~37 KB of heap, because the
 *    cart SRAM (64 KB), internal RAM (64 KB), external EEPROM (8 KB) and the
 *    320x240 framebuffer (153 KB) are all in on-chip RAM. The ROM was never in
 *    SRAM -- it was in flash -- so moving it to PSRAM does not free any of that,
 *    but it does remove the 4 MB ceiling and the re-flash cost.
 *
 * The image is byte-identical to what the flash path held, and the core is
 * read-only with respect to it: cpu_readmem20 is the only reader, cpu_writemem20
 * drops every bank above 1, and the Detective Conan self-modifying patch is
 * already virtualised into a 5-byte lookup table inside the read path
 * (src/oswan/memory.c). So pointing ws_rom at a PSRAM block changes nothing
 * about how the core sees the cartridge.
 *
 * Allocation goes through Frens::f_malloc (the allocator the PSRAM window was
 * registered with) and release through Frens::f_free. Never free() on these
 * blocks: plain free() comes from the SRAM heap, and handing it a lwmem block
 * corrupts the heap and hangs the MCU on the way out of a game. */
static bool ws_load_rom_to_psram(FIL *fil, uint32_t size, const char *filename) {
    if (size == 0) {
        printf("[E] Refusing to load a zero-byte ROM into PSRAM\n");
        return false;
    }

    /* Release the previous game FIRST, before the space check below.
     *
     * The ordering matters and was the bug that made a second ROM impossible
     * without rebooting: GetAvailableMemory() still counts the block that the
     * previous game occupies, so checking the new size against it rejected a
     * cartridge of exactly the size that had just loaded successfully -- and
     * because the early return happened above this free, the block was never
     * released either, so every later attempt failed the same way. Release
     * first, then the allocator sees the real free total. */
    if (s_rom_in_psram && s_rom_ptr) {
        printf("[I] Freeing previous PSRAM ROM at 0x%08x\n",
               (unsigned)(uintptr_t)s_rom_ptr);
        Frens::f_free(s_rom_ptr);
        s_rom_ptr = NULL;
        s_rom_in_psram = false;
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

    uint8_t *buf = (uint8_t *)Frens::f_malloc(size);
    if (!buf) {
        printf("[E] PSRAM allocation of %u bytes failed\n", (unsigned)size);
        return false;
    }

    /* Published before the read so a failed load is still recognisable. */
    s_rom_ptr = buf;
    s_rom_in_psram = true;

    dmg_draw_loading_screen(&s_ws_desc, "CARREGANDO WS ROM", filename, size);
    Frens::blinkLed(true);

    /* Read in chunks so the progress bar reflects real progress, and so a short
     * read stops the loop instead of leaving the tail of the buffer
     * uninitialised -- the NEC V30 would then execute whatever was in PSRAM. */
    const uint32_t PSRAM_READ_CHUNK = 16u * 1024u;
    uint32_t bytes_loaded = 0;
    bool ok = true;
    f_lseek(fil, 0);

    while (bytes_loaded < size) {
        watchdog_update();
        uint32_t want = size - bytes_loaded;
        if (want > PSRAM_READ_CHUNK) want = PSRAM_READ_CHUNK;
        UINT br = 0;
        FRESULT r = f_read(fil, buf + bytes_loaded, want, &br);
        if (r != FR_OK || br == 0) {
            printf("[E] f_read failed at %u/%u: %d\n",
                   (unsigned)bytes_loaded, (unsigned)size, (int)r);
            ok = false;
            break;
        }
        bytes_loaded += br;
        dmg_update_loading_progress(&s_ws_desc, bytes_loaded, size);
    }

    Frens::blinkLed(false);
    f_lseek(fil, 0);

    if (!ok) {
        Frens::f_free(buf);
        s_rom_ptr = NULL;
        s_rom_in_psram = false;
        f_close(fil);
        return false;
    }

    /* Same CRC the flash path records, so save states and the config file keep
     * working across the two paths. */
    uint32_t crc = 0;
    for (uint32_t i = 0; i < bytes_loaded; i++) crc = (crc << 1) ^ buf[i];
    Frens::setCrcOfLoadedRom(crc);

    /* Close here rather than leaving s_fil open for the whole session. The
     * flash path has always closed before returning (main.cpp:602); keeping it
     * open across the game meant the same FIL object was handed to f_open()
     * again on the next selection, with FatFs's per-file sector window still
     * live. Nothing reads the file during play, so this is free. */
    f_close(fil);

    printf("[I] ROM in PSRAM at 0x%08x: %u bytes\n",
           (unsigned)(uintptr_t)buf, (unsigned)bytes_loaded);
    s_last_flashed_size = bytes_loaded;
    return true;
}

static bool flash_load_rom(const char *filename) {
    const char *rdir = dmg_get_current_rom_dir();
    if (!rdir || rdir[0] == '\0') rdir = "0:/roms/ws";
    snprintf(s_full_path, sizeof(s_full_path), "%s/%s", rdir, filename);
    if (f_open(&s_fil, s_full_path, FA_READ) != FR_OK) {
        snprintf(s_full_path, sizeof(s_full_path), "0:/roms/ws/%s", filename);
        if (f_open(&s_fil, s_full_path, FA_READ) != FR_OK) return false;
    }

    FSIZE_t fsz = f_size(&s_fil);
    if (fsz < 512) { f_close(&s_fil); return false; }
    uint32_t size = (uint32_t)fsz;

    /* Console build: read the ROM straight into PSRAM. Placed before the cache
     * header check because that header describes the flash window, which this
     * path never writes. The 4 MB WS_ROM_CACHE_MAX ceiling applies only to the
     * flash window, so it is not checked here. */
    if (Frens::isPsramEnabled()) {
        return ws_load_rom_to_psram(&s_fil, size, filename);
    }

    if (size > WS_ROM_CACHE_MAX) { f_close(&s_fil); return false; }

    char clean[64];
    dmg_sanitize_basename(filename, clean, sizeof(clean));

    const PicoWS_Cache_Header_t *hdr = (const PicoWS_Cache_Header_t *)WS_CACHE_HDR_ABS;
    bool flashed = false;
    if (hdr->magic == WS_CACHE_MAGIC && hdr->rom_size == size &&
        strncmp(hdr->filename, clean, sizeof(clean)) == 0) {
        /* Spot-check 32 blocks */
        static uint8_t sbuf[512];
        bool ok = true;
        uint32_t hash = 2166136261u;
        for (int i = 0; i < 32 && ok; i++) {
            uint32_t off = (size > 512) ? (uint32_t)(((uint64_t)i * (size-512))/31) : 0;
            off &= ~3u;
            if (f_lseek(&s_fil, off) != FR_OK) { ok=false; break; }
            UINT br=0;
            if (f_read(&s_fil, sbuf, 512, &br) != FR_OK || br != 512) { ok=false; break; }
            if (memcmp((const void*)(WS_ROM_CACHE_ABS + off), sbuf, 512) != 0) { ok=false; break; }
            const uint32_t *p = (const uint32_t *)sbuf;
            for (int w=0;w<128;w++) hash = (hash ^ p[w]) * 16777619u;
        }
        if (ok && hash == hdr->sample_crc) flashed = true;
    }
    f_lseek(&s_fil, 0);

    if (!flashed) {
        dmg_draw_loading_screen(&s_ws_desc, "CARREGANDO FLASH", filename, size);

        /* 2 KB (was 4 KB): frees SRAM for the static I2S ring; any multiple
         * of FLASH_PAGE_SIZE works with the loop below. */
#if defined(PSRAM_CS_PIN) && PSRAM_CS_PIN
        static uint8_t blk_store[2048];
        uint8_t *blk = blk_store;
#else
        /* HW15 has no SRAM to spare: borrow the Tate frame buffer, which is
         * idle while a ROM is being loaded (it is only drawn during play). */
        uint8_t *blk = s_ws_vframe;
#endif
        const uint32_t blk_size = 2048u;
        uint32_t off = 0;
        dmg_boot_intro_flash_begin();   /* core 1 runs the intro from XIP */
        while (off < size) {
            watchdog_update();
            uint32_t chunk = (size - off > blk_size) ? blk_size : (size - off);
            UINT rd = 0;
            if (f_read(&s_fil, blk, chunk, &rd) != FR_OK || rd != chunk) {
                f_close(&s_fil); dmg_boot_intro_flash_end(); return false;
            }
            uint32_t prog = (chunk + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
            if (prog > chunk) memset(blk + chunk, 0xFF, prog - chunk);

            if ((off % FLASH_BLOCK_SIZE) == 0u) {
                safe_flash_range_erase(WS_ROM_CACHE_OFF + off, FLASH_BLOCK_SIZE);
            }
            for (uint32_t p = 0; p < prog; p += FLASH_PAGE_SIZE) {
                safe_flash_range_program(WS_ROM_CACHE_OFF + off + p, blk + p, FLASH_PAGE_SIZE);
            }

            if (memcmp((const void *)(WS_ROM_CACHE_ABS + off), blk, chunk) != 0) {
                printf("[E] Flash verification failed at offset %u!\n", (unsigned)off);
                f_close(&s_fil);
                dmg_boot_intro_flash_end();
                return false;
            }

            off += chunk;
            dmg_update_loading_progress(&s_ws_desc, off, size);
        }

        uint32_t crc = compute_ws_sample_hash((const uint8_t *)WS_ROM_CACHE_ABS, size);
        static PicoWS_Cache_Header_t nhdr __aligned(4);
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.magic      = WS_CACHE_MAGIC;
        nhdr.rom_size   = size;
        nhdr.sample_crc = crc;
        strncpy(nhdr.filename, clean, sizeof(nhdr.filename)-1);
        safe_flash_range_erase(WS_CACHE_HDR_OFF, FLASH_SECTOR_SIZE);
        safe_flash_range_program(WS_CACHE_HDR_OFF, (const uint8_t *)&nhdr, FLASH_PAGE_SIZE);
        dmg_boot_intro_flash_end();
    } else {
        dmg_draw_loading_screen(&s_ws_desc, "CARREGANDO FLASH", filename, size);
        dmg_update_loading_progress(&s_ws_desc, size, size);
        sleep_ms(150);
    }

    apply_safe_qmi_timing();
    f_close(&s_fil);
    s_last_flashed_size = size;
    return true;
}

static void draw_image_row(uint8_t idx, bool selected, ili9341_scale_mode_t scale, bool vertical, bool grid, bool shimmerless, bool interleave, uint8_t palette) {
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    dmg_palette_t pal;
    dmg_get_palette(&s_ws_desc, &pal);
    uint16_t bg = selected ? pal.sel : pal.bg;

    ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    char buf[44];
    buf[0] = '\0';

    if (idx == 0) {
        const char *scale_names[] = { "< Fit >", "< Stretch >", "< Integer >" };
        snprintf(buf, sizeof(buf), "Scale:        %s", scale_names[scale]);
    } else if (idx == 1) {
        snprintf(buf, sizeof(buf), "Orientation:  %s", vertical ? "< Vertical >" : "< Horizontal >");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Effect:       %s", grid ? "< Grid >" : "< None >");
    } else if (idx == 3) {
        snprintf(buf, sizeof(buf), "Filter:       %s", shimmerless ? "< Shimmerless >" : "< Sharp >");
    } else if (idx == 4) {
        snprintf(buf, sizeof(buf), "Screen:       %s", interleave ? "< Interleave >" : "< Progressive >");
    } else if (idx == 5) {
        if (ws_gpu_operatingInColor) {
            snprintf(buf, sizeof(buf), "Palette:      < N/A (Color) >");
        } else {
            const char *pal_names[] = { "< Default >", "< Amber >", "< Green >" };
            snprintf(buf, sizeof(buf), "Palette:      %s", pal_names[palette % 3]);
        }
    }

    ili9341_text(buf, wx + 12, (uint16_t)(y + 7u), selected ? pal.text_sel : pal.text, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void image_submenu(const char *game_name) {
    uint8_t cursor = 0;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;

    ili9341_scale_mode_t sel_scale = cfg_scale;
    bool sel_vertical = s_ws_vertical;
    bool sel_grid = cfg_grid;
    bool sel_shimmerless = cfg_shimmerless;
    bool sel_interleave = cfg_interleave;
    uint8_t sel_palette = cfg_palette;

    dmg_palette_t pal;
    dmg_get_palette(&s_ws_desc, &pal);

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    if (dmg_is_gb_mode()) {
        ili9341_fill_screen(0x0000);
        ili9341_fill_rect(wx, 0, ww, 240, pal.bg);
    } else {
        ili9341_fill_screen(pal.bg);
    }
    dmg_draw_top_bar(&s_ws_desc, APP_TITLE, "Image Settings");
    dmg_draw_game_bar(&s_ws_desc, game_name);
    dmg_draw_bottom_bar(&s_ws_desc, "L/R: Adjust", "B: Apply & Back");

    for (uint8_t i = 0; i < 6; i++) {
        draw_image_row(i, cursor == i, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
    }
    ili9341_flush_rows(DMG_UI_INGAME_LIST_START_Y + 6 * DMG_UI_ITEM_HEIGHT, 218);

    wait_buttons_released();

    while (true) {
        watchdog_update();
        bool up, down, left, right, a, b, sel, start;
        read_ui_buttons(&up, &down, &left, &right, &a, &b, &sel, &start);

        if (b && !prev_b) {
            cfg_scale = sel_scale;
            s_ws_vertical = sel_vertical;
            cfg_grid = sel_grid;
            cfg_shimmerless = sel_shimmerless;
            cfg_interleave = sel_interleave;
            cfg_palette = sel_palette;
            ili9341_set_scale_mode(cfg_scale);
            ili9341_set_ws_vertical(s_ws_vertical);
            ili9341_set_grid_effect(cfg_grid);
            ili9341_set_shimmerless(cfg_shimmerless);
            ili9341_set_interleave(cfg_interleave);
            if (!ws_gpu_operatingInColor) {
                ws_set_colour_scheme(cfg_palette);
            }
            ili9341_invalidate_ws_palette();
            save_config();
            break;
        }

        if (up && !prev_up) {
            if (cursor > 0) {
                draw_image_row(cursor, false, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
                cursor--;
                draw_image_row(cursor, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            }
        }
        if (down && !prev_down) {
            if (cursor < 5) {
                draw_image_row(cursor, false, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
                cursor++;
                draw_image_row(cursor, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            }
        }

        if (cursor == 0) {
            if ((left && !prev_left) || (a && !prev_a)) {
                sel_scale = (sel_scale == SCALE_FIT) ? SCALE_INTEGER : (ili9341_scale_mode_t)(sel_scale - 1);
                draw_image_row(0, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            } else if (right && !prev_right) {
                sel_scale = (sel_scale == SCALE_INTEGER) ? SCALE_FIT : (ili9341_scale_mode_t)(sel_scale + 1);
                draw_image_row(0, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            }
        } else if (cursor == 1) {
            if ((left && !prev_left) || (right && !prev_right) || (a && !prev_a)) {
                sel_vertical = !sel_vertical;
                draw_image_row(1, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            }
        } else if (cursor == 2) {
            if ((left && !prev_left) || (right && !prev_right) || (a && !prev_a)) {
                sel_grid = !sel_grid;
                draw_image_row(2, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            }
        } else if (cursor == 3) {
            if ((left && !prev_left) || (right && !prev_right) || (a && !prev_a)) {
                sel_shimmerless = !sel_shimmerless;
                draw_image_row(3, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            }
        } else if (cursor == 4) {
            if ((left && !prev_left) || (right && !prev_right) || (a && !prev_a)) {
                sel_interleave = !sel_interleave;
                draw_image_row(4, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
            }
        } else if (cursor == 5) {
            if (!ws_gpu_operatingInColor) {
                if ((left && !prev_left) || (right && !prev_right) || (a && !prev_a)) {
                    sel_palette = (sel_palette + 1) % 3;
                    draw_image_row(5, true, sel_scale, sel_vertical, sel_grid, sel_shimmerless, sel_interleave, sel_palette);
                }
            }
        }

        prev_up = up; prev_down = down; prev_left = left; prev_right = right;
        prev_a = a; prev_b = b;
        sleep_ms(20);
    }
    wait_buttons_released();
}

/* =========================================================================
 * In-game pause menu (shared_menu wrapper)
 * ========================================================================= */
typedef enum {
    INGAME_ACTION_RESUME = 0,
    INGAME_ACTION_RESET,
    INGAME_ACTION_EXIT
} ingame_action_e;

static ingame_action_e show_ingame_menu(const char *game_name) {
    watchdog_disable();
    ws_flush_battery_save();
    ws_trace_dump("0:/saves/ws/klonoa_trace.txt");   /* no-op unless WS_SND_TRACE=1 */
    dmg_action_t act = dmg_ingame_menu(&s_ws_desc, game_name);
    if (act == DMG_ACTION_RESUME || act == DMG_ACTION_RESET) {
        ili9341_clear_black();
        ili9341_blitter_start();
        watchdog_enable(6000, false);
    }
    return (ingame_action_e)act;
}

/* =========================================================================
 * Game loop
 * ========================================================================= */
static volatile bool s_req_pause = false;
static volatile bool s_req_exit  = false;

/* Interpose ws_executeLine: stream each rendered scanline to ILI9341 */
extern uint32_t ws_shades[16];

static void run_game(void) {
    apply_safe_qmi_timing();
    /* The console build holds the ROM in PSRAM (s_rom_ptr); the HW15 build
     * programs it into the flash XIP window. Same bytes, same contract:
     * ws_memory_init() stores the pointer verbatim and the core only ever
     * reads through it. */
    uint8_t *rom_base = s_rom_in_psram ? s_rom_ptr : (uint8_t *)WS_ROM_CACHE_ABS;
    ws_init(rom_base, s_last_flashed_size);
    ws_reset();
#if WS_SND_TRACE
    /* Optional sound/timer/interrupt trace (WS_SND_TRACE=1, see ws_snd_trace.h): 16384
     * entries in PSRAM, dumped to the card each time the pause menu opens. Allocated once. */
    static void *s_trace_buf = nullptr;
    const size_t trace_bytes = 16384u * 16u;
    if (s_trace_buf == nullptr && Frens::isPsramEnabled() &&
        (uint32_t)Frens::GetAvailableMemory() > trace_bytes + 1024u * 1024u) {
        s_trace_buf = Frens::f_malloc(trace_bytes);
        if (s_trace_buf) ws_trace_attach(s_trace_buf, trace_bytes);
    }
#endif
    ws_trace_reset();
    ws_romHeaderStruct *hdr = ws_rom_getHeader(rom_base, s_last_flashed_size);
    const char *ext = strrchr(s_selected_rom, '.');
    if ((hdr && hdr->minimumSupportSystem == WS_SYSTEM_COLOR) || (ext && strcasecmp(ext, ".wsc") == 0))
        ws_set_system(WS_SYSTEM_COLOR);
    else
        ws_set_system(WS_SYSTEM_MONO);
    /* Also on a colour system: a game can run in the console's mono mode (port 0x60 bit 7
     * clear) and then draws with the shade table. */
    ws_set_colour_scheme(cfg_palette);
    ili9341_invalidate_ws_palette();

    char clean_name[64];
    get_basename(s_selected_rom, clean_name, sizeof(clean_name));
    const char *sav_path = s_sav_path;
    snprintf(s_sav_path, sizeof(s_sav_path), "%s/%s.sav", get_saves_dir(), clean_name);
    ws_sram_load(sav_path);
    s_sav_epoch  = ws_sram_epoch;
    s_sav_active = true;

    /* Auto-detect if game is vertically oriented from ROM header */
    extern int ws_rotated(void);
    s_ws_vertical = ws_rotated() ? true : false;

#if defined(PSRAM_CS_PIN) && PSRAM_CS_PIN
    if (s_ws_vframe == nullptr) {
        s_ws_vframe = (uint8_t *)Frens::f_malloc(144 * 224);
        if (s_ws_vframe) memset(s_ws_vframe, 0, 144 * 224);
    }
#endif
    dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */
    ili9341_clear_black();
    ili9341_set_scale_mode(cfg_scale);
    ili9341_set_ws_vertical(s_ws_vertical);
    ili9341_set_grid_effect(cfg_grid);
    ili9341_set_shimmerless(cfg_shimmerless);
    ili9341_set_interleave(cfg_interleave);
    ili9341_set_fps_overlay(cfg_fps, "60");
    ili9341_blitter_start();

    s_req_pause = false;
    s_req_exit  = false;

    uint32_t fps_timer   = time_us_32();
    uint32_t frame_timer = time_us_32();
    uint32_t frame_cnt   = 0;
    int32_t  lag_us      = 0;   /* how far behind real time the last frame ended */
    uint8_t  skipped_in_row = 0;
    bool     audio_primed = false;  /* ring has reached its working level at least once */
    /* Frames since audio_primed was cleared. A game that runs slower than real time from
     * its first frame never fills the ring to its working level, so waiting for that alone
     * kept render-skip and audio stretch off for the whole session: the ring sat near
     * empty and clicked (Digimon at 574 cycles/line: fill 4%, ~280 underruns/s, 0 skips).
     * After 30 frames the startup / SD-write transient is over either way. */
    uint8_t  unprimed_frames = 0;
    /* In-game saves: the .sav used to be written only when the game was left, so switching
     * the console off after saving inside the game lost the save, while savestates (written
     * at once) survived. Now it is written ~2 s after the last change to SRAM/EEPROM, once
     * the game has stopped writing; a game that never stops writing is still saved every
     * 30 s. The pause menu and the power switch flush it too (ws_stop_blitter). */
    uint32_t sram_seen = ws_sram_epoch, sram_quiet_since = time_us_32(), sram_dirty_since = 0;
    /* Drop any half-finished SELECT+START hold or closed edge latch left over
     * from the previous game. */
    dmg_combo_reset();

    /* 60 Hz frame pacing matching ILI9341 TFT refresh (~16680 us) */
    const uint32_t BASE_FRAME_US = 16680u;

    watchdog_enable(6000, false);

    /* Framebuffer for scanline streaming */
    /* WS renders into internalRam regions; we capture scanlines in a static buffer */
    static uint8_t __aligned(4) s_scanline_buf[224];

    while (!s_req_exit) {
        watchdog_update();
        dmg_pad_t pad_in;
        read_ui_buttons(&pad_in.up, &pad_in.down, &pad_in.left, &pad_in.right,
                        &pad_in.a, &pad_in.b, &pad_in.select, &pad_in.start);

        /* Raw copies, read before the combo layer masks anything. The Tate
         * rotation below has to know whether SELECT was pressed *together with*
         * another button, and it is exactly those combinations the combo layer
         * clears -- so the raw values are the only ones that still carry it.
         * Reading the masked pad here would make SELECT+A rotate the screen
         * when SELECT is released. */
        const bool raw_sel   = pad_in.select;
        const bool raw_start = pad_in.start;
        const bool raw_a     = pad_in.a;
        const bool raw_b     = pad_in.b;
        const bool raw_up    = pad_in.up;
        const bool raw_down  = pad_in.down;

        /* Combos:
         * START + B: In-game pause menu
         * START + A: Toggle FPS overlay
         * SELECT + START: Exit to ROM selector (held ~1 s)
         * SELECT + A: Toggle 1x / 2x speed
         * SELECT + UP / DOWN: Volume adjustment
         * SELECT (single press): Toggle Horizontal / Vertical (Tate) orientation
         */
        dmg_combo_events_t ev;
        dmg_combo_frame(&pad_in, &s_ws_desc, &ev);

        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (ev.request_power_off) {
            ws_stop_blitter();
            dmg_power_off_with_resume(&s_ws_desc, (dmg_power_reason_t)ev.power_reason);
        }

        if (ev.request_pause_menu) s_req_pause = true;
        if (ev.request_exit_game)  s_req_exit = true;
        if (ev.speed_toggled) {
            printf("[I] Combo SELECT+A: speed = %s\n", cfg_turbo ? "2x" : "1x");
        }

        bool up = pad_in.up, down = pad_in.down;
        bool left = pad_in.left, right = pad_in.right;
        bool a = pad_in.a, b = pad_in.b;
        bool sel = pad_in.select, start = pad_in.start;

        /* Single SELECT press: Toggle Horizontal / Vertical orientation on release */
        static bool s_sel_combo_active = false;
        static bool s_last_sel = false;
        if (raw_sel) {
            if (raw_start || raw_up || raw_down || raw_a || raw_b) {
                s_sel_combo_active = true;
            }
        } else {
            if (s_last_sel && !s_sel_combo_active) {
                s_ws_vertical = !s_ws_vertical;
                ili9341_set_ws_vertical(s_ws_vertical);
                ili9341_clear_black();
            }
            s_sel_combo_active = false;
        }
        s_last_sel = raw_sel;

        {
            const uint32_t ep = ws_sram_epoch;
            const uint32_t tnow = time_us_32();
            if (ep == s_sav_epoch) {
                sram_dirty_since = tnow;
            } else {
                if (ep != sram_seen) { sram_seen = ep; sram_quiet_since = tnow; }
                if (tnow - sram_quiet_since > 2000000u || tnow - sram_dirty_since > 30000000u) {
                    ws_flush_battery_save();
                    sram_dirty_since = time_us_32();
                    audio_primed = false;   /* the SD write is a long frame; do not read it as the ring running dry */
                    unprimed_frames = 0;
                    frame_timer = time_us_32();
                }
            }
        }

        if (s_req_pause && !s_req_exit) {
            s_req_pause = false;
            ingame_action_e action = show_ingame_menu(s_selected_rom);
            if (action == INGAME_ACTION_EXIT)  { s_req_exit = true; break; }
            if (action == INGAME_ACTION_RESET) {
                ws_reset();
                /* Reload the battery save, exactly as game entry does.
                 *
                 * ws_reset() clears cartridge SRAM and EEPROM along with
                 * internalRam; the .sav was flushed when the menu opened, so it
                 * holds the latest in-game save.
                 *
                 * Reading the .sav back makes Reset deterministic and identical
                 * to entering the game from the ROM selector, which is what
                 * "restart with my progress" has to mean. This was the observed
                 * symptom: the save loaded from the selector but not from the
                 * menu's Reset.
                 *
                 * Note the .sav format (73728 B = ws_staticRam + externalEeprom)
                 * does not include internalRam, and that is deliberate -- it
                 * matches hardware, where work RAM is lost on reset and only the
                 * cartridge-backed saves survive.
                 *
                 * Returns false when there is no .sav, which is correct: a game
                 * played for the first time keeps the blank power-on state that
                 * ws_reset() just produced. */
                if (!ws_sram_load(sav_path)) {
                    printf("[I] No battery save at %s (or unreadable) -- fresh game\n",
                           sav_path);
                }
                frame_timer = time_us_32();
                fps_timer = time_us_32();
                frame_cnt = 0;
            }
            continue;
        }

        /* Feed input to WS */
        extern uint8_t ws_key_up, ws_key_down, ws_key_left, ws_key_right;
        extern uint8_t ws_key_button_1, ws_key_button_2, ws_key_start;
        ws_key_up      = (uint8_t)up;
        ws_key_down    = (uint8_t)down;
        ws_key_left    = (uint8_t)left;
        ws_key_right   = (uint8_t)right;
        ws_key_button_1= (uint8_t)a;
        ws_key_button_2= (uint8_t)b;
        ws_key_start   = (uint8_t)start;

        /* Render-skip: when emulation has fallen behind real time, the next frame is
         * emulated (so game speed and audio stay at 60 Hz) but not drawn; the panel just
         * keeps the previous image. Never more than 2 in a row. */
#if USE_I2S_AUDIO
        /* With audio the DAC is the clock: skip drawing only when its buffer is running dry */
        const int fill_now = audio_i2s_get_fill_permille();
        const bool behind = audio_primed && !cfg_turbo && fill_now < 300;
        /* A frame that ran late leaves the ring below its working level (~55% after the
         * pacing wait). Instead of letting it run dry -- the DMA stops and every gap is a
         * click -- the next frame produces more audio: up to +9% (800 samples) at 25%
         * fill. Same pitch, slightly slower tempo, exactly what the game is doing. */
        int spf = 735;
        if (audio_primed && !cfg_turbo && fill_now < 500) {
            const int f = (fill_now < 250) ? 250 : fill_now;
            spf = 735 + ((500 - f) * 65) / 250;
        }
        ws_audio_set_samples_per_frame(spf);
#else
        const bool behind = lag_us > 2500;
#endif
        const bool draw_frame = !(behind && skipped_in_row < 2);
        skipped_in_row = draw_frame ? 0 : (uint8_t)(skipped_in_row + 1);

        /* Emulate one frame: call ws_executeLine until VBL */
        /* oswan renders 144 visible lines + 15 blank = 159 total */
        int vbl = 0;
        while (!vbl) {
            vbl = ws_executeLine(s_scanline_buf, draw_frame ? 1 : 0);
            extern uint8_t ws_gpu_scanline;
            int line = (int)ws_gpu_scanline - 1;
            if (line < 0) line = 143;
            if (draw_frame && line < 144) {
                if (!s_ws_vertical) {
                    /* Horizontal mode: stream scanline directly */
                    ili9341_stream_ws_line(line, s_scanline_buf, ws_shades);
                } else {
                    /* Vertical mode: cache line into full frame buffer */
                    memcpy(s_ws_vframe + (size_t)line * 224, s_scanline_buf, 224);
                }
            }
        }

        if (draw_frame && s_ws_vertical) {
            /* Vertical (Tate) mode: 224 display lines, each 144 pixels */
            /* Rotated 90 degrees CCW: vx = 0..143, vy = 0..223 */
            /* ox = 223 - vy, oy = vx */
            static uint8_t __aligned(4) s_vline_buf[144];
            for (int vy = 0; vy < 224; vy++) {
                int ox = 223 - vy;
                for (int vx = 0; vx < 144; vx++) {
                    int oy = vx;
                    s_vline_buf[vx] = s_ws_vframe[oy * 224 + ox];
                }
                ili9341_stream_ws_vline(vy, s_vline_buf, ws_shades);
            }
        }
        if (draw_frame) ili9341_stream_ws_frame_end();
        frame_cnt++;

        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            watchdog_disable();
            ws_stop_blitter();
            const bool resumed = ws_load_state(s_selected_rom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_clear_black();
            ili9341_blitter_start();
            watchdog_enable(6000, false);
            frame_timer = time_us_32();
            fps_timer = time_us_32();
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }
        uint32_t now = time_us_32();
        if (now - fps_timer >= 1000000u) {
            uint32_t fps_val = frame_cnt;
            frame_cnt = 0;
            fps_timer = now;
            snprintf(s_fps_str, sizeof(s_fps_str), "%lu", (unsigned long)fps_val);
            if (cfg_fps) ili9341_set_fps_overlay(true, s_fps_str);
        }

        /* Frame pacing (Turbo: half time) */
        uint32_t frame_us = cfg_turbo ? (BASE_FRAME_US / 2) : BASE_FRAME_US;
#if USE_I2S_AUDIO
        if (!cfg_turbo) {
            /* Audio-clock pacing. The I2S DAC consumes exactly 44100 samples/s and each
             * frame produces 735, so holding its ring at ~half full locks the emulator to
             * the DAC with no drift, and leaves ~1 frame of headroom to absorb a slow
             * frame. Pacing by a wall-clock timer instead (16680 us vs the DAC's
             * 16667 us) kept the ring nearly empty: any frame that ran long drained it
             * and the DMA stalled -- the crackling heard on every frame-time wobble. */
            const uint32_t t_wait = time_us_32();
            int fill = audio_i2s_get_fill_permille();
            while (fill > 550 && (time_us_32() - t_wait) < 40000u) {
                watchdog_update();
                busy_wait_us_32(100);
                fill = audio_i2s_get_fill_permille();
            }
            if (!audio_primed && (fill >= 500 || ++unprimed_frames >= 30)) audio_primed = true;
            lag_us = 0;
            frame_timer = time_us_32();
        } else
#endif
        {
        uint32_t target = frame_timer + frame_us;
        uint32_t cur = time_us_32();
        if ((int32_t)(target - cur) > 0) busy_wait_us_32(target - cur);
        /* Being late is remembered (lag_us) instead of forgotten, so a frame that
         * misses its slot is paid back by the next ones; beyond 3 frames behind we
         * give up and resync (menu, SD hiccup...). */
        const int32_t behind = (int32_t)(time_us_32() - target);
        if (behind > 0) {
            lag_us = behind;
            frame_timer = (behind > (int32_t)(3 * frame_us)) ? time_us_32() : target;
            if (behind > (int32_t)(3 * frame_us)) lag_us = 0;
        } else {
            lag_us = 0;
            frame_timer = target;
        }
        }
    }

    watchdog_disable();
    ili9341_blitter_stop();
    ws_flush_battery_save();   /* final flush; the game is normally saved long before this */
    s_sav_active = false;
    ws_done();
    ili9341_clear_black();

}

/* =========================================================================
 * Main
 * ========================================================================= */
int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    set_sys_clock_khz(CPU_FREQ_KHZ, true);
    apply_safe_qmi_timing();

    /* Decouple SPI clock from CPU: PLL_USB → 140 MHz, SPI divisor 2 = 70 MHz */
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
    pll_init(pll_usb, 1, 840 * MHZ, 6, 1);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    140 * MHZ, 140 * MHZ);

    stdio_init_all();

#ifndef LED_GPIO_PIN
#define LED_GPIO_PIN 25
#endif
    /* LED */
    gpio_init(LED_GPIO_PIN);
    gpio_set_dir(LED_GPIO_PIN, GPIO_OUT);
    gpio_put(LED_GPIO_PIN, 1);

    /* Hardware Buttons (GPIO 2..9) */
    init_ui_buttons();

    /* Configure SD card on SPI1 (HW_CONFIG 15/16: MISO=12, CS=13, SCK=14, MOSI=15)
     *
     * This is now BEFORE ili9341_init(), which inverts the order task 8 of
     * AGENTS.md chose. That task moved the display up so an SD failure would be
     * visible instead of a silently blinking LED -- which was right then, but
     * ili9341_init() now calls dmg_panel_config_init() to read ST7789= from
     * /boot.txt, and that needs the card mounted. Both are satisfied by mounting
     * first and displaying the failure afterwards: the error screen below is
     * unchanged, it is just drawn after the panel is known. */
    static pico_fatfs_spi_config_t sd_config = {
        SDCARD_SPI,
        CLK_SLOW_DEFAULT,
        CLK_FAST_DEFAULT_PIO,
        SDCARD_PIN_MISO,
        SDCARD_PIN_CS,
        SDCARD_PIN_SCK,
        SDCARD_PIN_MOSI,
        true // use internal pullup
    };
    bool spi_configured = pico_fatfs_set_config(&sd_config);
    if (!spi_configured) {
        pico_fatfs_config_spi_pio(SDCARD_PIO, pio_claim_unused_sm(SDCARD_PIO, true));
    }

    /* Mount SD Card with retry */
    FRESULT fr = f_mount(&s_fs, "", 1);
    if (fr != FR_OK) {
        sleep_ms(200);
        fr = f_mount(&s_fs, "", 1);
    }

    /* Detect the PSRAM now that the card is mounted and the SD is quiescent.
     * Frens::initAll() is the usual caller, but PicoWS never calls it (it drives
     * the SD and display itself), so without this explicit call isPsramEnabled()
     * stays false and f_malloc would fall back to the SRAM heap. Compiled out
     * entirely when PSRAM_CS_PIN is 0, i.e. on the HW_CONFIG 15 board. */
#if PICO_RP2350 && PSRAM_CS_PIN
    if (Frens::initPsram()) {
        printf("[I] PSRAM detected: %u KB available for cartridge ROMs\n",
               (unsigned)(Frens::GetAvailableMemory() / 1024u));
    } else {
        printf("[W] PSRAM not detected; falling back to the flash ROM cache\n");
    }
#endif

    /* Display init. Now after the mount so dmg_panel can read /boot.txt. */
    ili9341_init();
    set_brightness(5);
    ili9341_clear_black();

    /* Show initialization screen */
    ili9341_fill_screen(UI_BG);
    ili9341_fill_rect(0, 0, 320, 20, UI_BAR);
    ili9341_text(APP_TITLE, 8, 6, UI_TBRT, UI_BAR);
    ili9341_text("Iniciando MicroSD...", 50, 110, UI_TXT, UI_BG);
    ili9341_flush_rows(0, 239);

    if (fr != FR_OK) {
        ili9341_fill_screen(UI_BG);
        ili9341_fill_rect(0, 0, 320, 20, UI_BAR);
        ili9341_text(APP_TITLE, 8, 6, UI_TBRT, UI_BAR);
        ili9341_text("Erro ao montar cartao MicroSD!", 20, 75, UI_ERR, UI_BG);
        char errbuf[48];
        snprintf(errbuf, sizeof(errbuf), "Codigo FatFS: %d", (int)fr);
        ili9341_text(errbuf, 20, 100, UI_TXT, UI_BG);
        ili9341_text("SPI1: MISO=12 CS=13 SCK=14 MOSI=15", 20, 125, UI_TXT, UI_BG);
        ili9341_text("Verifique se ha pasta /roms/ws/", 20, 150, UI_TXT, UI_BG);
        ili9341_flush_rows(0, 239);
        while (true) {
            gpio_put(LED_GPIO_PIN, !gpio_get(LED_GPIO_PIN));
            sleep_ms(300);
            bool u, d, l, r, a, b, sel, sta;
            read_ui_buttons(&u, &d, &l, &r, &a, &b, &sel, &sta);
            if (a || b || sta || sel) {
                watchdog_reboot(0, 0, 0);
                while (1) tight_loop_contents();
            }
        }
    }

    dmg_init_boot_mode();
    load_config();
#if USE_I2S_AUDIO
    audio_i2s_setup(USE_I2S_AUDIO, 44100, -1);
#endif
    set_sound_volume_level(cfg_volume);
    set_brightness(cfg_bright);
    ili9341_set_grid_effect(cfg_grid);
    ili9341_set_fps_overlay(cfg_fps, "60");
    ensure_save_dirs();

    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_ws_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_ws_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_ws_desc);
            snprintf(s_selected_rom, sizeof(s_selected_rom), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_ws_desc)) dmg_boot_intro_finish();
        }
    }

    printf("\n=== %s WonderSwan for RP2350 ===\n", APP_TITLE);
    printf("CPU: %lu MHz | Display: ILI9341 320x240\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000));

    if (!s_resume_pending) s_selected_rom[0] = '\0';

    while (true) {
        if (s_resume_pending || rom_file_selector(s_selected_rom, sizeof(s_selected_rom))) {
            if (flash_load_rom(s_selected_rom)) {
                wait_buttons_released();
                dmg_power_set_game(s_selected_rom, s_selected_rom);
                run_game();
                dmg_power_set_game(NULL, NULL);
            } else {
                if (s_resume_pending) {
                    s_resume_pending = false;
                    dmg_resume_consume();
                }
                dmg_boot_intro_finish();
                /* Show error */
                ili9341_fill_screen(UI_BG);
                ili9341_text("ROM load failed!", 80, 120, UI_ERR, UI_BG);
                ili9341_flush_rows(0, 239);
                sleep_ms(2000);
            }
        }
    }

    return 0;
}
