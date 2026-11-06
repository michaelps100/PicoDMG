#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/divider.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/pwm.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/pll.h"

#include "util/work_meter.h"
#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"
#include "InfoNES_Region.h"
#include "ff.h"
#include "tusb.h"
#include "gamepad.h"
#include "rom_selector.h"
#include "menu.h"
#include "nespad.h"
#include "wiipad.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "FrensFonts.h"
#include "vumeter.h"
#include "menu_settings.h"
#include "state.h"
#include "soundrecorder.h"
#include "pico/bootrom.h"
#include "InfoNES_FDS.h"
#include "InfoNES_NSF.h"
#include "zapper.h"
#if USE_ILI9341
#include "ili9341.h"
#include "crc32.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#endif
#if EMBEDDED_NES_ROM
extern "C" const unsigned char embedded_nes_rom[];
extern "C" const unsigned int embedded_nes_rom_len;
#endif
bool isFatalError = false;
//static bool pendingLoadState = false;
char *romName;
bool showSettings = false;
bool loadSaveStateMenu = false;
SaveStateTypes quickSaveAction = SaveStateTypes::NONE;
static uint32_t start_tick_us = 0;
static uint32_t fps = 0;
static uint8_t framesbeforeAutoStateIsLoaded = 0;
#define EMULATOR_CLOCKFREQ_KHZ 252000 // CPU frequency in kHz (252 MHz; SPI decoupled via PLL_USB @ 160 MHz -> 80 MHz)


// Note: When using framebuffer, AUDIOBUFFERSIZE must be increased to 1024
#if PICO_RP2350
#define AUDIOBUFFERSIZE 1024
#else
#define AUDIOBUFFERSIZE 256
#endif

// DVI Gain (Q8). 256 = 1.0x, 384 = 1.5x, 512 = 2.0x
#ifndef DVI_AUDIO_GAIN_Q8
#define DVI_AUDIO_GAIN_Q8 1024
#endif
// Current gain setting (DVI audio)
static int g_dvi_audio_gain_q8 = DVI_AUDIO_GAIN_Q8;

// Recording gain (Q8). 256 = 1.0x, 512 = 2.0x
#ifndef RECORD_GAIN_Q8
#define RECORD_GAIN_Q8 2048
#endif
static int g_record_gain_q8 = RECORD_GAIN_Q8;

static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;
// Visibility configuration for options menu (NES specific)
// 1 = show option line, 0 = hide.
// Order must match enum in menu_options.h
#if PICO_RP2350
static const MenuFdsHooks fdsMenuHooks = {
    fdsCurrentSwapValue,
    fdsGetNumSides,
    fdsRequestSwap,
    fdsRequestEject
};
#endif

// Non-const so the FDS disk-swap entry can be flipped on per-ROM after
// fdsParse() detects we're loading a .fds. The pointer in
// `g_settings_visibility` is `const int8_t *`, but it can point at
// non-const storage just fine.
int8_t g_settings_visibility_nes[MOPT_COUNT] = {
    [MOPT_EXIT_GAME]               = 0,                    // Exit Game, or back to menu. Always visible when in-game.
    [MOPT_RESET_GAME]              = 0,                    // Reset Game
    [MOPT_REBOOT_TO_LOADER]        = BOOTLOADER_BUILD,     // Return to emuLoader picker (only when built for the loader)
    [MOPT_SAVE_RESTORE_STATE]      = 0,                    // Save / Restore State
    [MOPT_SCREENMODE]              = 1,                    // Screen Mode
    [MOPT_SCANLINES]               = USE_ILI9341,          // Scanlines toggle
    [MOPT_SCANLINE_TYPE]           = HSTX,                 // Scanline Type (HSTX only)
    [MOPT_FPS_OVERLAY]             = 1,                    // FPS Overlay
    [MOPT_AUDIO_ENABLE]            = 0,                    // Audio Enable
    [MOPT_FRAMESKIP]               = 0,                    // Frame Skip
    [MOPT_DISPLAY_MODE]            = HSTX && ENABLEDVI,    // Display Mode (HDMI or DVI, only when HSTX is enabled, because non-HSTX builds always use HDMI)
    [MOPT_EXTERNAL_AUDIO]          = EXT_AUDIO_IS_ENABLED, // External Audio
    [MOPT_FONT_COLOR]              = 1,                    // Font Color
    [MOPT_FONT_BACK_COLOR]         = 1,                    // Font Back Color
    [MOPT_FRUITJAM_VUMETER]        = ENABLE_VU_METER,      // VU Meter
    [MOPT_FRUITJAM_VOLUME_CONTROL] = (HW_CONFIG == 8),     // Fruit Jam Volume Control
    [MOPT_DMG_PALETTE]             = 0,                    // DMG Palette (NES emulator does not use GameBoy palettes)
    [MOPT_BORDER_MODE]             = 0,                    // Border Mode (Super Gameboy style borders not applicable for NES)
    [MOPT_RAPID_FIRE_ON_A]         = 1,                    // Rapid Fire on A
    [MOPT_RAPID_FIRE_ON_B]         = 1,                    // Rapid Fire on B
    [MOPT_AUTO_INSERT_FDS_DISK_A]  = 0,                    // Auto Insert Disk A, enabled at runtime on RP2350
    [MOPT_AUTO_SWAP_FDS_DISK]      = 0,                    // Auto Swap FDS, enabled at runtime on RP2350
    [MOPT_FDS_DISK_SWAP]           = 0,                    // FDS Disk Swap (toggled on after fdsParse succeeds)
    [MOPT_OVERCLOCK]               = 0,                    // Overclock (CPU high clock toggle set at runtime, depends on HSTX and PSRAM available)
    [MOPT_FM_AUDIO]                = 0,                    // YM2413 FM (SMS only, RP2350-only with HSTX)
    [MOPT_ENTER_BOOTSEL_MODE]      = 1,                    // Enter bootsel mode
    [MOPT_CONTROLLER_TEST]         = 1,                    // Controller test
    [MOPT_RECENT_GAMES]            = 0,                    // Recently played (menu.cpp force-shows this in the rom browser)
    [MOPT_USB_DRIVE_MODE]          = 0,                    // USB drive mode (menu.cpp force-shows this in the rom browser)
    [MOPT_CASSETTE]                = 0,                    // Cassette deck (TI-99/4A only)
    [MOPT_DISK]                    = 0,                    // Disk drives (TI-99/4A only)
    [MOPT_SERIAL_KEYBOARD]         = 0,                    // Serial console keyboard (TI-99/4A only)
};
// #if defined(__riscv)
// const uint8_t g_available_screen_modes[] = {
//     0, // SCANLINE_8_7,      
//     0, // NOSCANLINE_8_7,
//     1, // SCANLINE_1_1,
//     1  // NOSCANLINE_1_1
// };
// #else
const uint8_t g_available_screen_modes_nes[] = {
    1, // SCANLINE_8_7,
    1, // NOSCANLINE_8_7,
    1, // SCANLINE_1_1,
    1  // NOSCANLINE_1_1
    };
//#endif

namespace
{
    ROMSelector romSelector_;
}

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
// Cached Wii pad state updated once per frame in ProcessAfterFrameIsRendered()
static uint16_t wiipad_raw_cached = 0;
#endif
#if 0
#if !HSTX
// convert RGB565 to RGB444
#define CC(x) (((x >> 1) & 15) | (((x >> 6) & 15) << 4) | (((x >> 11) & 15) << 8))
#else 
// convert RGB565 to RGB555
#define CC(x) ((((x) >> 11) & 0x1F) << 10 | (((x) >> 6) & 0x1F) << 5 | ((x) & 0x1F))
#endif
const WORD __not_in_flash_func(NesPalette)[64] = {
    CC(0x39ce), CC(0x1071), CC(0x0015), CC(0x2013), CC(0x440e), CC(0x5402), CC(0x5000), CC(0x3c20),
    CC(0x20a0), CC(0x0100), CC(0x0140), CC(0x00e2), CC(0x0ceb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x5ef7), CC(0x01dd), CC(0x10fd), CC(0x401e), CC(0x5c17), CC(0x700b), CC(0x6ca0), CC(0x6521),
    CC(0x45c0), CC(0x0240), CC(0x02a0), CC(0x0247), CC(0x0211), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x1eff), CC(0x2e5f), CC(0x223f), CC(0x79ff), CC(0x7dd6), CC(0x7dcc), CC(0x7e67),
    CC(0x7ae7), CC(0x4342), CC(0x2769), CC(0x2ff3), CC(0x03bb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x579f), CC(0x635f), CC(0x6b3f), CC(0x7f1f), CC(0x7f1b), CC(0x7ef6), CC(0x7f75),
    CC(0x7f94), CC(0x73f4), CC(0x57d7), CC(0x5bf9), CC(0x4ffe), CC(0x0000), CC(0x0000), CC(0x0000)};
#endif
#if 1
#if USE_ILI9341
// RGB888 to RGB555 (15-bit RGB: bit 15 is 0, keeping it clean for InfoNES backdrop flag 0x8000!)
#define CC(c) (((c & 0xf8) >> 3) | ((c & 0xf800) >> 6) | ((c & 0xf80000) >> 9))
const WORD __not_in_flash_func(NesPalette)[64] = {
    CC(0x626262), CC(0x001C95), CC(0x1904AC), CC(0x42009D),
    CC(0x61006B), CC(0x6E0025), CC(0x650500), CC(0x491E00),
    CC(0x223700), CC(0x004900), CC(0x004F00), CC(0x004816),
    CC(0x00355E), CC(0x000000), CC(0x000000), CC(0x000000),

    CC(0xABABAB), CC(0x0C4EDB), CC(0x3D2EFF), CC(0x7115F3),
    CC(0x9B0BB9), CC(0xB01262), CC(0xA92704), CC(0x894600),
    CC(0x576600), CC(0x237F00), CC(0x008900), CC(0x008332),
    CC(0x006D90), CC(0x000000), CC(0x000000), CC(0x000000),

    CC(0xFFFFFF), CC(0x57A5FF), CC(0x8287FF), CC(0xB46DFF),
    CC(0xDF60FF), CC(0xF863C6), CC(0xF8746D), CC(0xDE9020),
    CC(0xB3AE00), CC(0x81C800), CC(0x56D522), CC(0x3DD36F),
    CC(0x3EC1C8), CC(0x4E4E4E), CC(0x000000), CC(0x000000),

    CC(0xFFFFFF), CC(0xBEE0FF), CC(0xCDD4FF), CC(0xE0CAFF),
    CC(0xF1C4FF), CC(0xFCC4EF), CC(0xFDCACE), CC(0xF5D4AF),
    CC(0xE6DF9C), CC(0xD3E99A), CC(0xC2EFA8), CC(0xB7EFC4),
    CC(0xB6EAE5), CC(0xB8B8B8), CC(0x000000), CC(0x000000)};
#elif !HSTX
// RGB565 to RGB444
#define CC(x) (((x >> 1) & 15) | (((x >> 6) & 15) << 4) | (((x >> 11) & 15) << 8))
const WORD __not_in_flash_func(NesPalette)[64] = {
    CC(0x39ce), CC(0x1071), CC(0x0015), CC(0x2013), CC(0x440e), CC(0x5402), CC(0x5000), CC(0x3c20),
    CC(0x20a0), CC(0x0100), CC(0x0140), CC(0x00e2), CC(0x0ceb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x5ef7), CC(0x01dd), CC(0x10fd), CC(0x401e), CC(0x5c17), CC(0x700b), CC(0x6ca0), CC(0x6521),
    CC(0x45c0), CC(0x0240), CC(0x02a0), CC(0x0247), CC(0x0211), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x1eff), CC(0x2e5f), CC(0x223f), CC(0x79ff), CC(0x7dd6), CC(0x7dcc), CC(0x7e67),
    CC(0x7ae7), CC(0x4342), CC(0x2769), CC(0x2ff3), CC(0x03bb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x579f), CC(0x635f), CC(0x6b3f), CC(0x7f1f), CC(0x7f1b), CC(0x7ef6), CC(0x7f75),
    CC(0x7f94), CC(0x73f4), CC(0x57d7), CC(0x5bf9), CC(0x4ffe), CC(0x0000), CC(0x0000), CC(0x0000)};
#else
// RGB888 to RGB555
#define CC(c) (((c & 0xf8) >> 3) | ((c & 0xf800) >> 6) | ((c & 0xf80000) >> 9))
const WORD __not_in_flash_func(NesPalette)[64] = {
    CC(0x626262), CC(0x001C95), CC(0x1904AC), CC(0x42009D),
    CC(0x61006B), CC(0x6E0025), CC(0x650500), CC(0x491E00),
    CC(0x223700), CC(0x004900), CC(0x004F00), CC(0x004816),
    CC(0x00355E), CC(0x000000), CC(0x000000), CC(0x000000),

    CC(0xABABAB), CC(0x0C4EDB), CC(0x3D2EFF), CC(0x7115F3),
    CC(0x9B0BB9), CC(0xB01262), CC(0xA92704), CC(0x894600),
    CC(0x576600), CC(0x237F00), CC(0x008900), CC(0x008332),
    CC(0x006D90), CC(0x000000), CC(0x000000), CC(0x000000),

    CC(0xFFFFFF), CC(0x57A5FF), CC(0x8287FF), CC(0xB46DFF),
    CC(0xDF60FF), CC(0xF863C6), CC(0xF8746D), CC(0xDE9020),
    CC(0xB3AE00), CC(0x81C800), CC(0x56D522), CC(0x3DD36F),
    CC(0x3EC1C8), CC(0x4E4E4E), CC(0x000000), CC(0x000000),

    CC(0xFFFFFF), CC(0xBEE0FF), CC(0xCDD4FF), CC(0xE0CAFF),
    CC(0xF1C4FF), CC(0xFCC4EF), CC(0xFDCACE), CC(0xF5D4AF),
    CC(0xE6DF9C), CC(0xD3E99A), CC(0xC2EFA8), CC(0xB7EFC4),
    CC(0xB6EAE5), CC(0xB8B8B8), CC(0x000000), CC(0x000000)};
#endif
#endif
/* True when ROM_FILE_ADDR points at a PSRAM buffer this file allocated, so it is
 * ours to free. It must not be freed otherwise: in the flash path ROM_FILE_ADDR
 * is an XIP address and Frens::f_free() on one of those corrupts the heap.
 * Declared here because getCurrentNVRAMAddr() below keys off it. */
static bool s_rom_in_psram = false;

uint32_t getCurrentNVRAMAddr()
{

    if (!romSelector_.getCurrentROM())
    {
        return {};
    }
    // The NVRAM slot is carved out of the flash region just below the ROM, so
    // this arithmetic only means anything for a flash-resident ROM. With a
    // PSRAM ROM the subtraction would point at unrelated PSRAM contents and be
    // memcpy'd in as a battery save, so report "no NVRAM here" and let the
    // caller start from a clean SRAM. Saves live on the SD card regardless.
    if (s_rom_in_psram)
    {
        return {};
    }
    int slot = romSelector_.getCurrentNVRAMSlot();
    if (slot < 0)
    {
        return {};
    }
    printf("SRAM slot %d\n", slot);
    return ROM_FILE_ADDR - SRAM_SIZE * (slot + 1);
}

static void ensure_save_directories(void) {
    f_mkdir("0:/saves");
    f_mkdir("0:/saves/nes");
    f_mkdir("/saves");
    f_mkdir("/saves/nes");
}

void saveNVRAM()
{
    char pad[FF_MAX_LFN];
    char fileName[FF_MAX_LFN];
    strcpy(fileName, Frens::GetfileNameFromFullPath(romName));
    Frens::stripextensionfromfilename(fileName);
#if PICO_RP2350
    if (IsFDS)
    {
        ensure_save_directories();
        snprintf(pad, FF_MAX_LFN, "0:/saves/nes/%s_fds", fileName);
        fdsSaveSidecar(pad);
        return;
    }
#endif
    if (!SRAMwritten)
    {
        printf("SRAM not updated.\n");
        return;
    }
    ensure_save_directories();
    snprintf(pad, FF_MAX_LFN, "0:/saves/nes/%s.sav", fileName);
    printf("Save SRAM to %s\n", pad);
    FIL fil;
    FRESULT fr = f_open(&fil, pad, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        snprintf(pad, FF_MAX_LFN, "/saves/nes/%s.sav", fileName);
        fr = f_open(&fil, pad, FA_CREATE_ALWAYS | FA_WRITE);
    }
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot open save file: %d", fr);
        printf("%s\n", ErrorMessage);
        return;
    }
    size_t bytesWritten;
    fr = f_write(&fil, SRAM, SRAM_SIZE, &bytesWritten);
    if (bytesWritten < SRAM_SIZE)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Error writing save: %d %d/%d written", fr, bytesWritten, SRAM_SIZE);
        printf("%s\n", ErrorMessage);
    }
    f_close(&fil);
    printf("done\n");
    SRAMwritten = false;
}

bool loadNVRAM()
{
    char pad[FF_MAX_LFN];
    FILINFO fno;
    bool ok = false;
    char fileName[FF_MAX_LFN];
    strcpy(fileName, Frens::GetfileNameFromFullPath(romName));
    Frens::stripextensionfromfilename(fileName);

#if PICO_RP2350
    if (IsFDS)
    {
        snprintf(pad, FF_MAX_LFN, "0:/saves/nes/%s_fds", fileName);
        fdsSetSaveBasePath(pad);
        return fdsLoadSidecar(pad);
    }
#endif

    snprintf(pad, FF_MAX_LFN, "0:/saves/nes/%s.sav", fileName);
    FRESULT fr = f_stat(pad, &fno);
    if (fr != FR_OK) {
        snprintf(pad, FF_MAX_LFN, "0:/saves/nes/%s.SAV", fileName);
        fr = f_stat(pad, &fno);
    }
    if (fr != FR_OK) {
        snprintf(pad, FF_MAX_LFN, "%s/%s.SAV", GAMESAVEDIR, fileName);
        fr = f_stat(pad, &fno);
    }

    FIL fil;
    size_t bytesRead;
    if (auto addr = getCurrentNVRAMAddr())
    {
        printf("Load SRAM from %s\n", pad);
        if (fr == FR_NO_FILE)
        {
            printf("Save file not found, load SRAM from flash %x\n", addr);
            memcpy(SRAM, reinterpret_cast<void *>(addr), SRAM_SIZE);
            ok = true;
        }
        else
        {
            if (fr == FR_OK)
            {
                printf("Loading save file %s\n", pad);
                fr = f_open(&fil, pad, FA_READ);
                if (fr == FR_OK)
                {
                    fr = f_read(&fil, SRAM, SRAM_SIZE, &bytesRead);
                    if (fr == FR_OK)
                    {
                        printf("Savefile read from disk\n");
                        ok = true;
                    }
                    else
                    {
                        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot read save file: %d %d/%d read", fr, bytesRead, SRAM_SIZE);
                        printf("%s\n", ErrorMessage);
                    }
                    f_close(&fil);
                }
                else
                {
                    snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot open save file: %d", fr);
                    printf("%s\n", ErrorMessage);
                }
            }
            else
            {
                snprintf(ErrorMessage, ERRORMESSAGESIZE, "f_stat() failed on save file: %d", fr);
                printf("%s\n", ErrorMessage);
            }
        }
    }
    else
    {
        ok = true;
    }
    SRAMwritten = false;
    return ok;
}

/* =========================================================================
 * Persistent Configuration (picones.cfg)
 * ========================================================================= */
#define PICONES_CONFIG_MAGIC 0x53454E50u /* "PNES" */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t scale_mode;        /* 0 = Fit, 1 = Stretch, 2 = Integer */
    uint32_t scanlines_enabled; /* 0 = Normal, 1 = Scanlines */
    uint32_t display_fps;       /* 0 = Off, 1 = On */
    uint32_t volume_level;      /* 1..5, default 3 */
    uint32_t brightness_level;  /* 1..5, default 5 */
    uint32_t turbo_mode;        /* 0 = Normal 1x, 1 = Turbo 2x */
    uint32_t save_slot;         /* 1..4, default 1 */
    uint32_t load_slot;         /* 1..4, default 1 */
    uint32_t interleave;        /* 0 = Progressive, 1 = Interleave */
    uint32_t crop_sides;        /* 0 = Off, 1 = 8px Mask (L+R), 2 = 8px Fill */
    uint32_t height_fill;       /* 0 = Native 240p, 1 = Fill Screen */
    uint32_t filter_mode;       /* 0 = Shimmerless, 1 = Sharp */
} picones_config_t;

static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_scanlines_enabled = false;
static bool persistent_shimmerless_enabled = true;
static bool persistent_interleave = true;
static uint8_t persistent_crop_sides = 1;       /* 0 = Off, 1 = 8px Mask (L+R), 2 = 8px Fill */
static bool persistent_height_fill = true;
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
static bool s_resume_load_pending = false;
static bool s_request_exit_game = false;

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

static void save_picones_config(void) {
    ensure_save_directories();
    FIL fil;
    FRESULT fr = f_open(&fil, "0:/saves/nes/picones.cfg", FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        fr = f_open(&fil, "/saves/nes/picones.cfg", FA_WRITE | FA_CREATE_ALWAYS);
    }
    if (fr == FR_OK) {
        picones_config_t cfg;
        cfg.magic = PICONES_CONFIG_MAGIC;
        cfg.version = 1;
        cfg.scale_mode = (uint32_t)persistent_scale_mode;
        cfg.scanlines_enabled = persistent_scanlines_enabled ? 1u : 0u;
        cfg.display_fps = persistent_display_fps ? 1u : 0u;
        cfg.volume_level = (uint32_t)persistent_volume_level;
        cfg.brightness_level = (uint32_t)persistent_brightness_level;
        cfg.turbo_mode = (uint32_t)persistent_turbo_mode;
        cfg.save_slot = (uint32_t)persistent_save_slot;
        cfg.load_slot = (uint32_t)persistent_load_slot;
        cfg.interleave = persistent_interleave ? 1u : 0u;
        cfg.crop_sides = (uint32_t)persistent_crop_sides;
        cfg.height_fill = persistent_height_fill ? 1u : 0u;
        cfg.filter_mode = persistent_shimmerless_enabled ? 0u : 1u;
        UINT bw;
        f_write(&fil, &cfg, sizeof(cfg), &bw);
        f_close(&fil);
    }
}

static void load_picones_config(void) {
    FIL fil;
    FRESULT fr = f_open(&fil, "0:/saves/nes/picones.cfg", FA_READ);
    if (fr != FR_OK) {
        fr = f_open(&fil, "/saves/nes/picones.cfg", FA_READ);
    }
    if (fr == FR_OK) {
        picones_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        UINT br;
        fr = f_read(&fil, &cfg, sizeof(cfg), &br);
        f_close(&fil);
        if (fr == FR_OK && br >= sizeof(uint32_t) * 10 && cfg.magic == PICONES_CONFIG_MAGIC) {
            if (cfg.scale_mode <= (uint32_t)SCALE_INTEGER) {
                persistent_scale_mode = (ili9341_scale_mode_t)cfg.scale_mode;
                ili9341_set_scale_mode(persistent_scale_mode);
            }
            persistent_scanlines_enabled = (cfg.scanlines_enabled != 0u);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            persistent_display_fps = (cfg.display_fps != 0u);
            settings.flags.displayFrameRate = persistent_display_fps;
            if (cfg.volume_level <= 5) {
                persistent_volume_level = (uint8_t)cfg.volume_level;
                set_sound_volume_level(persistent_volume_level);
            }
            if (cfg.brightness_level >= 1 && cfg.brightness_level <= 5) {
                persistent_brightness_level = (uint8_t)cfg.brightness_level;
                set_backlight_brightness(persistent_brightness_level);
            }
            persistent_turbo_mode = (cfg.turbo_mode != 0u) ? 1 : 0;
            if (cfg.save_slot >= 1 && cfg.save_slot <= 4) persistent_save_slot = (uint8_t)cfg.save_slot;
            if (cfg.load_slot >= 1 && cfg.load_slot <= 4) persistent_load_slot = (uint8_t)cfg.load_slot;
            if (br >= offsetof(picones_config_t, interleave) + sizeof(uint32_t)) {
                persistent_interleave = (cfg.interleave != 0u);
                ili9341_set_interleave(persistent_interleave);
            }
            if (br >= offsetof(picones_config_t, crop_sides) + sizeof(uint32_t)) {
                persistent_crop_sides = (uint8_t)cfg.crop_sides;
                if (persistent_crop_sides > 2) persistent_crop_sides = 1;
                g_crop_sides = persistent_crop_sides;
                ili9341_set_crop_sides(persistent_crop_sides);
            }
            if (br >= offsetof(picones_config_t, height_fill) + sizeof(uint32_t)) {
                persistent_height_fill = (cfg.height_fill != 0u);
                ili9341_set_height_fill(persistent_height_fill);
            }
            if (br >= offsetof(picones_config_t, filter_mode) + sizeof(uint32_t)) {
                persistent_shimmerless_enabled = (cfg.filter_mode == 0u);
            } else {
                persistent_shimmerless_enabled = true;
            }
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            printf("I PicoNES config loaded successfully\n");
        }
    }
}

/* =========================================================================
 * Clean Light UI via shared_menu
 * ========================================================================= */
typedef dmg_action_t ingame_action_e;
#define INGAME_ACTION_RESUME DMG_ACTION_RESUME
#define INGAME_ACTION_RESET  DMG_ACTION_RESET
#define INGAME_ACTION_EXIT   DMG_ACTION_EXIT
#define init_ui_buttons      dmg_init_ui_buttons

static const char *s_nes_candidate_dirs[] = {
    "0:/roms/nes",
    "/roms/nes",
    "roms/nes",
    "0:/ROMS/NES",
    "/ROMS/NES",
    "ROMS/NES",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};

static bool is_nes_rom(const char *fname) {
    if (fname == NULL || fname[0] == '\0' || fname[0] == '.') return false;
    const char *dot = strrchr(fname, '.');
    if (dot == NULL) return false;
    return (strcasecmp(dot, ".nes") == 0 ||
            strcasecmp(dot, ".fds") == 0 ||
            strcasecmp(dot, ".nsf") == 0);
}

static uint32_t last_flashed_rom_size = 0;

/* Loads the ROM into PSRAM instead of programming it into flash.
 *
 * The flash path below computes the target as ROM_FILE_ADDR - XIP_BASE, which
 * is only meaningful when initAll() actually assigned ROM_FILE_ADDR. It does
 * that on the flash branch only; with a PSRAM present it leaves ROM_FILE_ADDR at
 * 0 and reports the size limit as psramMemorySize instead. Subtracting XIP_BASE
 * from 0 wraps to 0xF0000000, which flash_range_erase() rejects -- the load dies
 * on the first chunk, before the progress bar has moved. That is why the ROM
 * selector looked fine and the bar never filled.
 *
 * Reads in chunks rather than one f_read() of the whole file so the progress
 * bar reflects real progress, and so a short read stops the loop instead of
 * leaving the tail of the buffer uninitialised.
 */
static bool nes_load_rom_to_psram(const struct dmg_emulator_desc *desc,
                                  FIL *fil, uint32_t rom_file_size,
                                  const char *display_name) {
    if (rom_file_size == 0) {
        printf("[E] Refusing to load a zero-byte ROM into PSRAM\n");
        return false;
    }

    // f_malloc() panics on failure, so the size has to be checked up front
    // rather than discovered by the allocator. The margin covers the menu's
    // own scratch allocations plus lwmem bookkeeping.
    const uint32_t margin = 512u * 1024u;
    const uint32_t avail = (uint32_t)Frens::GetAvailableMemory();
    if (avail > margin && rom_file_size > (avail - margin)) {
        printf("[E] ROM needs %u bytes, only %u usable in PSRAM\n",
               (unsigned)rom_file_size, (unsigned)(avail - margin));
        return false;
    }

    // Release the previous game before allocating, otherwise switching between
    // ROMs leaks the whole of each one and eventually starves the allocator.
    if (s_rom_in_psram && ROM_FILE_ADDR != 0) {
        printf("[I] Freeing previous PSRAM ROM at 0x%08x\n",
               (unsigned)ROM_FILE_ADDR);
        Frens::f_free((void *)ROM_FILE_ADDR);
        ROM_FILE_ADDR = 0;
        s_rom_in_psram = false;
    }

    uint8_t *buf = (uint8_t *)Frens::f_malloc(rom_file_size);
    if (!buf) {
        printf("[E] PSRAM allocation of %u bytes failed\n", (unsigned)rom_file_size);
        return false;
    }

    // Published before the read: the loader and the emulator both key off
    // ROM_FILE_ADDR, and a failed load has to be able to recognise it.
    ROM_FILE_ADDR = (uintptr_t)buf;
    s_rom_in_psram = true;

    dmg_draw_loading_screen(desc, "CARREGANDO NES ROM", display_name, rom_file_size);
    Frens::blinkLed(true);

    const uint32_t PSRAM_READ_CHUNK = 16u * 1024u;
    uint32_t bytes_loaded = 0;
    bool ok = true;
    f_lseek(fil, 0);

    while (bytes_loaded < rom_file_size) {
        uint32_t want = rom_file_size - bytes_loaded;
        if (want > PSRAM_READ_CHUNK) want = PSRAM_READ_CHUNK;
        UINT br = 0;
        FRESULT fr = f_read(fil, buf + bytes_loaded, want, &br);
        if (fr != FR_OK || br == 0) {
            printf("[E] f_read failed at %u/%u: %d\n",
                   (unsigned)bytes_loaded, (unsigned)rom_file_size, (int)fr);
            ok = false;
            break;
        }
        bytes_loaded += br;
        dmg_update_loading_progress(desc, bytes_loaded, rom_file_size);
    }

    Frens::blinkLed(false);

    if (!ok) {
        Frens::f_free(buf);
        ROM_FILE_ADDR = 0;
        s_rom_in_psram = false;
        return false;
    }

    const int crcOffset = 16;
    uint32_t final_crc = 0;
    if (bytes_loaded > (uint32_t)crcOffset) {
        final_crc = update_crc32(0, buf + crcOffset, bytes_loaded - crcOffset);
    }
    Frens::setCrcOfLoadedRom(final_crc);
    printf("[I] ROM in PSRAM at 0x%08x: %u bytes, CRC: 0x%08X\n",
           (unsigned)ROM_FILE_ADDR, (unsigned)bytes_loaded,
           (unsigned)Frens::getCrcOfLoadedRom());
    return true;
}

static bool nes_on_load_rom(const struct dmg_emulator_desc *desc, const char *filename) {
    char full_path[256];
    const char *curr_dir = dmg_get_current_rom_dir();
    if (strchr(filename, '/') != NULL || curr_dir == NULL || curr_dir[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "%s", filename);
    } else {
        size_t dlen = strlen(curr_dir);
        if (dlen > 0 && curr_dir[dlen - 1] == '/') {
            snprintf(full_path, sizeof(full_path), "%s%s", curr_dir, filename);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", curr_dir, filename);
        }
    }

    printf("[I] ROM loading starts: %s\n", full_path);

    ili9341_blitter_stop();

    FIL fil;
    FRESULT fr = f_open(&fil, full_path, FA_READ);
    if (fr != FR_OK) {
        printf("E f_open(%s) error: %d\n", full_path, fr);
        return false;
    }

    uint32_t rom_file_size = (uint32_t)f_size(&fil);

    // A PSRAM board never programs the ROM into flash: flash_range_erase() and
    // flash_range_program() are destructive and slow, and the flash window on
    // this console is also where the loader image lives. Route to PSRAM before
    // touching flash_offset, which is meaningless without an XIP base.
    if (Frens::isPsramEnabled()) {
        bool ok = nes_load_rom_to_psram(desc, &fil, rom_file_size, filename);
        f_close(&fil);
        if (ok && !dmg_boot_intro_active()) {
            ili9341_fill_screen(0x0000);
            ili9341_flush(true);
        }
        return ok;
    }

    const uint8_t *flash_rom = (const uint8_t *)ROM_FILE_ADDR;
    uint32_t flash_offset = (uint32_t)(ROM_FILE_ADDR - XIP_BASE);

    /* Is this ROM already in flash? With the size known from this session a
     * first-sector check is enough. After a reboot the size is unknown (0) --
     * the normal case on a standby resume -- so the whole file is compared,
     * which stops at the first differing sector for another ROM and saves a
     * full erase/program (and a flash lockout of the intro) for the same one. */
    bool already_flashed = false;
    if (rom_file_size > 0 &&
        (last_flashed_rom_size == rom_file_size || last_flashed_rom_size == 0)) {
        const bool full = (last_flashed_rom_size == 0);
        static uint8_t chk[FLASH_SECTOR_SIZE];
        uint32_t off = 0;
        already_flashed = true;
        while (off < rom_file_size) {
            UINT cbr = 0;
            fr = f_read(&fil, chk, sizeof(chk), &cbr);
            if (fr != FR_OK || cbr == 0 || memcmp(chk, flash_rom + off, cbr) != 0) {
                already_flashed = false;
                break;
            }
            off += cbr;
            if (!full) break;
            watchdog_update();
        }
        f_lseek(&fil, 0);
        if (already_flashed) last_flashed_rom_size = rom_file_size;
    }

    if (already_flashed) {
        printf("[I] ROM already in flash, skipping write\n");
    } else {
        printf("[I] ROM changed or not flashed, programming flash...\n");
        dmg_draw_loading_screen(desc, "CARREGANDO NES ROM", filename, rom_file_size);
        Frens::blinkLed(true);
        #define FLASH_CHUNK_SIZE (16u * 1024u)
        static uint8_t block_buf[FLASH_CHUNK_SIZE];
        uint32_t bytes_loaded = 0;
        uint32_t cur_flash_offset = flash_offset;
        UINT br;

        while (bytes_loaded < rom_file_size) {
            memset(block_buf, 0xFF, FLASH_CHUNK_SIZE);
            fr = f_read(&fil, block_buf, FLASH_CHUNK_SIZE, &br);
            if (fr != FR_OK || br == 0) {
                break;
            }

            const uint32_t erase_len = (br + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
            const uint32_t prog_len = (br + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
            /* The resume intro runs from XIP on core 1: park it in RAM. */
            dmg_boot_intro_flash_begin();
            Frens::flashEraseSafe(cur_flash_offset, erase_len);
            Frens::flashProgramSafe(cur_flash_offset, block_buf, prog_len);
            dmg_boot_intro_flash_end();

            bytes_loaded += br;
            cur_flash_offset += erase_len;

            dmg_update_loading_progress(desc, bytes_loaded, rom_file_size);
        }
        last_flashed_rom_size = rom_file_size;
        Frens::blinkLed(false);
    }
    f_close(&fil);

    uint32_t final_crc = 0;
    int crcOffset = 16;
    if (rom_file_size > (uint32_t)crcOffset) {
        final_crc = update_crc32(0, flash_rom + crcOffset, rom_file_size - crcOffset);
    }
    Frens::setCrcOfLoadedRom(final_crc);
    printf("[I] ROM ready in flash: %u bytes, CRC: 0x%08X\n", (unsigned)rom_file_size, (unsigned)Frens::getCrcOfLoadedRom());

    ili9341_fill_screen(0x0000);
    ili9341_flush(true);
    return true;
}

static void draw_image_row(uint8_t idx, bool selected) {
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
        snprintf(buf, sizeof(buf), "Effect:       %s", persistent_scanlines_enabled ? "< Scanlines >" : "< Normal >");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Filter:       %s", persistent_shimmerless_enabled ? "< Shimmerless >" : "< Sharp >");
    } else if (idx == 3) {
        snprintf(buf, sizeof(buf), "Display:      %s", persistent_interleave ? "< Interleave >" : "< Progressive >");
    } else if (idx == 4) {
        const char *crop_names[] = { "< Off >", "< 8px (Mask) >", "< 8px (Fill) >" };
        snprintf(buf, sizeof(buf), "Crop Sides:   %s", crop_names[persistent_crop_sides % 3]);
    } else if (idx == 5) {
        snprintf(buf, sizeof(buf), "Height:       %s", persistent_height_fill ? "< Fill Screen >" : "< Native 240p >");
    }

    ili9341_text(buf, (uint16_t)(wx + 12u), (uint16_t)(y + 7u), selected ? DMG_RGB555_TEXT_SEL_COLOR : DMG_RGB555_TEXT_COLOR, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void nes_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0; /* 0..5 */
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
    dmg_draw_top_bar(desc, desc->app_title, "Image Settings");
    dmg_draw_game_bar(desc, game_name);
    dmg_draw_bottom_bar(desc, "L/R / A: Adjust", "B: Back");

    for (uint8_t i = 0; i < 6; i++) {
        draw_image_row(i, i == cursor);
    }
    for (uint8_t i = 6; i < 8; i++) {
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
            cursor = (cursor == 0) ? 5 : (cursor - 1);
            draw_image_row(prev, false);
            draw_image_row(cursor, true);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == 5) ? 0 : (cursor + 1);
            draw_image_row(prev, false);
            draw_image_row(cursor, true);
        }

        if (pushed_left || pushed_right || pushed_a) {
            if (cursor == 0) {
                if (pushed_right || pushed_a) {
                    persistent_scale_mode = (ili9341_scale_mode_t)((persistent_scale_mode + 1) % 3);
                } else {
                    persistent_scale_mode = (ili9341_scale_mode_t)((persistent_scale_mode == 0) ? 2 : (persistent_scale_mode - 1));
                }
                ili9341_set_scale_mode(persistent_scale_mode);
                save_picones_config();
                draw_image_row(0, true);
            } else if (cursor == 1) {
                persistent_scanlines_enabled = !persistent_scanlines_enabled;
                ili9341_set_scanlines(persistent_scanlines_enabled);
                save_picones_config();
                draw_image_row(1, true);
            } else if (cursor == 2) {
                persistent_shimmerless_enabled = !persistent_shimmerless_enabled;
                ili9341_set_shimmerless(persistent_shimmerless_enabled);
                save_picones_config();
                draw_image_row(2, true);
            } else if (cursor == 3) {
                persistent_interleave = !persistent_interleave;
                ili9341_set_interleave(persistent_interleave);
                save_picones_config();
                draw_image_row(3, true);
            } else if (cursor == 4) {
                if (pushed_right || pushed_a) {
                    persistent_crop_sides = (persistent_crop_sides + 1) % 3;
                } else {
                    persistent_crop_sides = (persistent_crop_sides == 0) ? 2 : (persistent_crop_sides - 1);
                }
                g_crop_sides = persistent_crop_sides;
                ili9341_set_crop_sides(persistent_crop_sides);
                save_picones_config();
                draw_image_row(4, true);
            } else if (cursor == 5) {
                persistent_height_fill = !persistent_height_fill;
                ili9341_set_height_fill(persistent_height_fill);
                save_picones_config();
                draw_image_row(5, true);
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

static bool nes_save_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/nes", game_name, slot, state_path, sizeof(state_path));
    ensure_save_directories();
    return (Emulator_SaveState(state_path) == 0);
}

static bool nes_load_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/nes", game_name, slot, state_path, sizeof(state_path));
    int res = Emulator_LoadState(state_path);
    if (res != 0 && slot == 1) {
        char legacy_path[DMG_STATE_PATH_MAX];
        char base[64];
        const char *slash = strrchr(game_name, '/');
        const char *src = slash ? (slash + 1) : game_name;
        strncpy(base, src, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(legacy_path, sizeof(legacy_path), "0:/saves/nes/%s.ss", base);
        res = Emulator_LoadState(legacy_path);
        if (res != 0) {
            snprintf(legacy_path, sizeof(legacy_path), "/saves/%s.ss", base);
            res = Emulator_LoadState(legacy_path);
        }
    }
    return (res == 0);
}

/* Slot probe for the shared menu's Load State row.
 *
 * The generic lookup covers <saves_dir>/<game>_<slot>.ss, which is the numbered
 * file. Slot 1 additionally falls back to the unnumbered <game>.ss, exactly as
 * nes_load_state() does, so that fallback is repeated here: without it the row
 * would grey out slot 1 for a card whose only state predates the numbering. */
static bool nes_state_exists(const char *game_name, uint8_t slot) {
    if (slot < 1 || slot > 4) return false;

    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/nes", game_name, slot, state_path, sizeof(state_path));
    if (dmg_state_file_present(state_path)) return true;

    if (slot == 1) {
        char base[64];
        dmg_sanitize_basename(game_name, base, sizeof(base));

        char legacy_path[DMG_STATE_PATH_MAX];
        snprintf(legacy_path, sizeof(legacy_path), "0:/saves/nes/%s.ss", base);
        if (dmg_state_file_present(legacy_path)) return true;

        snprintf(legacy_path, sizeof(legacy_path), "/saves/%s.ss", base);
        if (dmg_state_file_present(legacy_path)) return true;
    }
    return false;
}

static void nes_apply_scale(dmg_scale_mode_t mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void nes_apply_effect(bool scanlines) {
    persistent_scanlines_enabled = scanlines;
    ili9341_set_scanlines(persistent_scanlines_enabled);
}

static void nes_apply_interleave(bool interleave) {
    persistent_interleave = interleave;
    ili9341_set_interleave(persistent_interleave);
}

static void nes_apply_speed(bool turbo) {
    persistent_turbo_mode = turbo ? 1 : 0;
}

static void nes_apply_volume(uint8_t vol) {
    set_sound_volume_level(vol);
}

static void nes_apply_brightness(uint8_t br) {
    set_backlight_brightness(br);
}

static void nes_apply_filter(bool shimmerless) {
    persistent_shimmerless_enabled = shimmerless;
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

/* FPS overlay hooks, defined after the descriptor that stores them. */
#if USE_ILI9341
static bool nes_get_fps(void);
static void nes_apply_fps(bool on);
#endif

#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
static int nes_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void nes_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void nes_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(44100);
}
#endif

static const dmg_emulator_desc_t s_nes_desc = {
    .app_title            = "PicoNES",
    .default_rom_dir      = "0:/roms/nes",
    .candidate_dirs       = s_nes_candidate_dirs,
    .config_path          = "0:/saves/nes/picones.cfg",
    .config_fallback      = "/saves/nes/picones.cfg",
    .saves_dir            = "0:/saves/nes",
    .config_magic         = PICONES_CONFIG_MAGIC,
    .is_rgb555            = true,
    .display = {
        .fill_rect        = ili9341_fill_rect,
        .draw_text        = ili9341_text,
        .flush_rows       = ili9341_flush_rows,
        .fill_screen      = ili9341_fill_screen,
        .flush            = ili9341_flush,
        .stop_blitter     = ili9341_blitter_stop,
        .start_blitter    = ili9341_blitter_start,
#if USE_ILI9341
        .write_rect_565   = ili9341_write_rect_565
#endif
    },
    .is_rom_file          = is_nes_rom,
    .on_load_rom          = nes_on_load_rom,
    .save_state           = nes_save_state,
    .load_state           = nes_load_state,
    .apply_scale          = nes_apply_scale,
    .apply_effect         = nes_apply_effect,
    .apply_interleave     = nes_apply_interleave,
    .apply_speed          = nes_apply_speed,
    .apply_volume         = nes_apply_volume,
    .apply_brightness     = nes_apply_brightness,
    .apply_filter         = nes_apply_filter,
    .reset_game           = NULL,
    .exit_game            = saveNVRAM,
    .custom_submenu_title = "Image >",
    .custom_submenu       = nes_image_submenu,
    /* save_picones_config(), not dmg_config_save(): the PicoNES config is a
     * private struct with fields (crop_sides, height_fill) that dmg_config_t
     * has no room for, so the shared writer would silently drop them. */
    .save_config          = save_picones_config,
#if USE_ILI9341
    /* The FPS overlay is drawn by the ILI9341 driver, which only exists on the
     * console build. On the HDMI/desktop build these stay NULL, which the combo
     * layer treats as "no overlay to toggle" rather than an error. */
    .get_fps              = nes_get_fps,
    .apply_fps            = nes_apply_fps,
#endif
    /* Designated last because that is where it sits in the struct, and this file
     * is C++. It applies to both builds: the state file is on the SD either way,
     * so the Load State row greys the same slots. */
    .state_exists         = nes_state_exists,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    /* Resume intro chime, pushed from core 1 into the I2S ring. */
    .audio_free           = nes_audio_free,
    .audio_push           = nes_audio_push,
    .audio_restore        = nes_audio_restore
#endif
};

#if USE_ILI9341
static bool nes_get_fps(void) {
    return persistent_display_fps;
}

static void nes_apply_fps(bool on) {
    /* No driver call here: on the ILI9341 path the overlay is drawn by the
     * InfoNES display hook reading settings.flags.displayFrameRate, which is
     * what the START+A handler did before the combo layer existed. Calling an
     * ili9341_set_fps_overlay() would be wrong, since that driver has no such
     * entry point in this build -- the string is formatted per frame where the
     * overlay is composited (see the fpsString block in the draw path). */
    persistent_display_fps = on;
    settings.flags.displayFrameRate = persistent_display_fps;
}
#endif

static void rom_file_selector(char *selectedRom) {
    dmg_rom_selector(&s_nes_desc, selectedRom, 256);
}

static dmg_action_t ingame_menu(const char *game_name) {
    return dmg_ingame_menu(&s_nes_desc, game_name);
}

static DWORD prevButtons[2]{};
static int rapidFireMask[2]{};
static int rapidFireCounter = 0;
static bool reset = false;
static bool resetGame = false;
void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
    static constexpr int LEFT = 1 << 6;
    static constexpr int RIGHT = 1 << 7;
    static constexpr int UP = 1 << 4;
    static constexpr int DOWN = 1 << 5;
    static constexpr int SELECT = 1 << 2;
    static constexpr int START = 1 << 3;
    static constexpr int A = 1 << 0;
    static constexpr int B = 1 << 1;

    // moved variables outside function body because prevButtons gets initialized to 0 everytime the function is called.
    // This is strange because a static variable inside a function is only initialsed once and retains it's value
    // throughout different function calls.
    // Am i missing something?
    // static DWORD prevButtons[2]{};
    // static int rapidFireMask[2]{};
    // static int rapidFireCounter = 0;

    ++rapidFireCounter;

#if NES_PIN_CLK != -1
    // Buttons of a GPIO pad in NES order. A NES pad shifts them out that way
    // already; a SNES pad puts its B and Y in the A and B slots, so its face
    // buttons are named instead of taken positionally: physical A drives NES A
    // and physical B drives NES B, the same as on USB and Wii Classic pads, and
    // the same buttons the menu uses to choose and go back. X, Y, L and R have
    // no NES equivalent and are ignored there too.
    auto nespadGameBits = [](int padnum) -> int
    {
        if (nespad_padtype[padnum] != NESPAD_TYPE_SNES)
        {
            return nespad_states[padnum];
        }
        const uint16_t ext = nespad_states_ext[padnum];
        int v = ext & (SELECT | START | UP | DOWN | LEFT | RIGHT); // same bits on both pads
        if (ext & (1u << 8)) v |= A;
        if (ext & (1u << 0)) v |= B;
        return v;
    };
#endif

    bool usbConnected = false;
    for (int i = 0; i < 2; ++i)
    {
        auto &dst = i == 0 ? *pdwPad1 : *pdwPad2;
        auto &gp = io::getCurrentGamePadState(i);
        if (i == 0)
        {
            usbConnected = gp.isConnected();
        }
        int v = (gp.buttons & io::GamePadState::Button::LEFT ? LEFT : 0) |
                (gp.buttons & io::GamePadState::Button::RIGHT ? RIGHT : 0) |
                (gp.buttons & io::GamePadState::Button::UP ? UP : 0) |
                (gp.buttons & io::GamePadState::Button::DOWN ? DOWN : 0) |
                (gp.buttons & io::GamePadState::Button::A ? A : 0) |
                (gp.buttons & io::GamePadState::Button::B ? B : 0) |
                (gp.buttons & io::GamePadState::Button::SELECT ? SELECT : 0) |
                (gp.buttons & io::GamePadState::Button::START ? START : 0) |
                0;
#if NES_PIN_CLK != -1
        // When USB controller is connected both NES ports act as controller 2
        if (usbConnected)
        {
            if (i == 1)
            {
                v = v | nespadGameBits(1) | nespadGameBits(0);
            }
        }
        else
        {
            v |= nespadGameBits(i);
        }
#endif

// When USB controller is connected  wiipad acts as controller 2
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        if (usbConnected)
        {
            if (i == 1)
            {
                v |= wiipad_raw_cached;
            }
        }
        else // if no USB controller is connected, wiipad acts as controller 1
        {
            if (i == 0)
            {
                v |= wiipad_raw_cached;
            }
        }
#endif

#if USE_ILI9341
        if (i == 0)
        {
            static bool picodmg_buttons_inited = false;
            if (!picodmg_buttons_inited)
            {
                for (int pin = 2; pin <= 9; pin++)
                {
                    gpio_init(pin);
                    gpio_set_dir(pin, GPIO_IN);
                    gpio_pull_up(pin);
                }
                picodmg_buttons_inited = true;
            }
            if (!gpio_get(2)) v |= UP;
            if (!gpio_get(3)) v |= DOWN;
            if (!gpio_get(4)) v |= LEFT;
            if (!gpio_get(5)) v |= RIGHT;
            if (!gpio_get(6)) v |= A;
            if (!gpio_get(7)) v |= B;
            if (!gpio_get(8)) v |= SELECT;
            if (!gpio_get(9)) v |= START;
        }
#endif

        // BOOTSEL is entered only from pico-bootLoader (SELECT+START+UP held at
        // power-on), not from inside a game.

        // Combos (player 1 only): SELECT+START leaves the game after a ~1 s
        // hold, START+B opens the pause menu, START+A toggles the FPS overlay,
        // SELECT+A toggles 1x/2x, SELECT+UP/DOWN is the volume. Read from the
        // merged `v` rather than only from GPIO, so a USB or Wii pad gets the
        // same shortcuts as the console buttons.
        dmg_combo_events_t nes_combo;
        if (i == 0)
        {
            dmg_pad_t pad_in;
            pad_in.up     = (v & UP) != 0;
            pad_in.down   = (v & DOWN) != 0;
            pad_in.left   = (v & LEFT) != 0;
            pad_in.right  = (v & RIGHT) != 0;
            pad_in.a      = (v & A) != 0;
            pad_in.b      = (v & B) != 0;
            pad_in.select = (v & SELECT) != 0;
            pad_in.start  = (v & START) != 0;

            dmg_combo_frame(&pad_in, &s_nes_desc, &nes_combo);

            // Write the masked pad back: a combo that consumed a button must
            // not reach the emulated NES as a stray press.
            v = 0;
            if (pad_in.up)     v |= UP;
            if (pad_in.down)   v |= DOWN;
            if (pad_in.left)   v |= LEFT;
            if (pad_in.right)  v |= RIGHT;
            if (pad_in.a)      v |= A;
            if (pad_in.b)      v |= B;
            if (pad_in.select) v |= SELECT;
            if (pad_in.start)  v |= START;

            if (nes_combo.request_exit_game) s_request_exit_game  = true;
            if (nes_combo.request_pause_menu) s_request_pause_menu = true;
            if (nes_combo.request_power_off) {
                s_request_power_off = true;
                s_power_reason = nes_combo.power_reason;
            }
            if (nes_combo.speed_toggled) {
                printf("[I] Combo SELECT+A: speed = %s\n",
                       persistent_turbo_mode ? "2x" : "1x");
            }
        }

        int rv = v;
        rapidFireMask[i] = (settings.flags.rapidFireOnA ? A : 0) |
                           (settings.flags.rapidFireOnB ? B : 0);
        if (rapidFireCounter & 2)
        {
            // 15 fire/sec
            rv &= ~rapidFireMask[i];
        }

        dst = rv;

        auto p1 = v;

        auto pushed = v & ~prevButtons[i];

        if (p1 & START)
        {
            if (pushed & LEFT) {
#if HW_CONFIG == 8
               settings.fruitjamVolumeLevel = std::max(-63, settings.fruitjamVolumeLevel - 1);
               EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            } else if (pushed & RIGHT) {
#if HW_CONFIG == 8
               settings.fruitjamVolumeLevel = std::min(23, settings.fruitjamVolumeLevel + 1);
               EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            }
        }
        if (p1 & SELECT && !IsNSF)
        {
#if !USE_ILI9341
            if (pushed & START)
            {
                // saveNVRAM();
                // reset = true;
                FrensSettings::savesettings();
                showSettings = true;
            }
#endif
            if (pushed & B)
            {
               rapidFireMask[i] ^= io::GamePadState::Button::B;
            }
            if (pushed & LEFT)
            {
                // Toggle audio output, ignore if HSTX is enabled, because HSTX must use external audio
#if EXT_AUDIO_IS_ENABLED && !HSTX
                settings.flags.useExtAudio = !settings.flags.useExtAudio;
                if (settings.flags.useExtAudio)
                {
                    printf("Using I2S Audio\n");
                }
                else
                {
                    printf("Using DVIAudio\n");
                }

#else
                settings.flags.useExtAudio = 0;
#endif
                //FrensSettings::savesettings();
            }
#if ENABLE_VU_METER
            else if (pushed & RIGHT)
            {
                settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
                //FrensSettings::savesettings();
                // printf("VU Meter %s\n", settings.flags.enableVUMeter ? "enabled" : "disabled");
                turnOffAllLeds();
            }
#endif
        }

        prevButtons[i] = v;
    }

    /* NSF track controls (player 1 only, checked after button state update) */
    if (IsNSF)
    {
        static constexpr int LEFT = 1 << 6;
        static constexpr int RIGHT = 1 << 7;
        static constexpr int START = 1 << 3;
        static constexpr int SELECT = 1 << 2;
        static constexpr int A_BTN = 1 << 0;
        static constexpr int B_BTN = 1 << 1;

        /* Recalculate pushed for player 1: edges since last frame. */
        static DWORD nsfPrevPad = 0;
        DWORD nsfPushed = prevButtons[0] & ~nsfPrevPad;
        nsfPrevPad = prevButtons[0];

        if (!(prevButtons[0] & START) && !(prevButtons[0] & SELECT))
        {
            if (nsfPushed & RIGHT)
            {
                nsfNextTrack();
                /* Trigger a re-init by resetting the CPU state */
                resetGame = true;
            }
            else if (nsfPushed & LEFT)
            {
                nsfPrevTrack();
                resetGame = true;
            }
        }

        /* A = play, B = stop */
        if (nsfPushed & A_BTN)
            nsfStartPlayback();
        if (nsfPushed & B_BTN)
            nsfStopPlayback();

        if ((prevButtons[0] & SELECT) && (nsfPushed & START))
        {
            /* Select+Start: exit NSF playback, return to menu */
            reset = true;
        }
    }

    if (reset && !IsNSF)
    {
        saveNVRAM();
    }
    *pdwSystem = (reset || resetGame) ? PAD_SYS_QUIT : 0;
}

void InfoNES_MessageBox(const char *pszMsg, ...)
{
    printf("[MSG]");
    va_list args;
    va_start(args, pszMsg);
    vprintf(pszMsg, args);
    va_end(args);
    printf("\n");
}

void InfoNES_Error(const char *pszMsg, ...)
{
    printf("[Error]");
    va_list args;
    va_start(args, pszMsg);
    vsnprintf(ErrorMessage, ERRORMESSAGESIZE, pszMsg, args);
    printf("%s", ErrorMessage);
    va_end(args);
    printf("\n");
}
bool parseROM(const uint8_t *nesFile)
{
#if PICO_RP2350
    // Famicom Disk System dispatch. The disk image was loaded into memory
    // (PSRAM or flash); look up its size from the file on SD so we can
    // determine side count and strip any fwNES header.
    if (fdsIsFdsFilename(romName))
    {
        FILINFO fno;
        if (f_stat(romName, &fno) != FR_OK)
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot stat FDS file %s", romName);
            printf("%s\n", ErrorMessage);
            return false;
        }
        if (!fdsParse((BYTE *)nesFile, (size_t)fno.fsize))
        {
            // fdsParse already populated ErrorMessage via InfoNES_Error.
            return false;
        }
        // Disk image lives in memory at nesFile; PRG/CHR-RAM live in dedicated
        // FDS_* buffers. ROM/VROM are wired up by Mapper 20 init (phase 3).
        ROM = nullptr;
        VROM = nullptr;
        // Phase 5: expose FDS options in the in-game settings menu.
        g_settings_visibility_nes[MOPT_FDS_DISK_SWAP] = 1;
        g_settings_visibility_nes[MOPT_AUTO_SWAP_FDS_DISK] = 1;
        g_settings_visibility_nes[MOPT_AUTO_INSERT_FDS_DISK_A] = 1;
        menuSetFdsHooks(&fdsMenuHooks);
        return true;
    }
#endif

    // NSF (Nintendo Sound Format) detection — check magic or file extension.
    if (checkNSFMagic(nesFile))
    {
        // If already in NSF mode (e.g. track change via resetGame), skip re-parse
        // so that NsfCurrentTrack is preserved.
        if (IsNSF)
            return true;

        // Determine file size via f_stat (works for both SD and flash-based files).
        size_t fileSize = 0;
        FILINFO fno;
        if (f_stat(romName, &fno) == FR_OK)
            fileSize = (size_t)fno.fsize;
        if (fileSize == 0)
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot determine NSF file size");
            return false;
        }
        if (!nsfParse(nesFile, fileSize))
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "NSF parse error");
            return false;
        }
        ROM = nullptr;
        VROM = nullptr;
        return true;
    }

    memcpy(&NesHeader, nesFile, sizeof(NesHeader));
    if (!checkNESMagic(NesHeader.byID))
    {
        return false;
    }

    nesFile += sizeof(NesHeader);

    memset(SRAM, 0, SRAM_SIZE);

    if (NesHeader.byInfo1 & 4)
    {
        memcpy(&SRAM[0x1000], nesFile, 512);
        nesFile += 512;
    }

    auto romSize = NesHeader.byRomSize * 0x4000;
    auto vromSize = NesHeader.byVRomSize * 0x2000;

    // Detect ROMs where the header overstates PRG size and CHR data is
    // embedded inside the declared PRG area (e.g. Galaxian (J) has 8KB PRG +
    // 8KB CHR packed as 16KB with a header claiming 16KB PRG + 8KB CHR).
    // The reset vector at the end of the declared PRG will be invalid while
    // the vector at (romSize - vromSize) is valid.
    if (romSize > 0 && vromSize > 0 && romSize > vromSize)
    {
        uint16_t resetVec = nesFile[romSize - 4] | ((uint16_t)nesFile[romSize - 3] << 8);
        if (resetVec < 0x8000)
        {
            auto actualPrgSize = romSize - vromSize;
            uint16_t fixedResetVec = nesFile[actualPrgSize - 4] |
                                     ((uint16_t)nesFile[actualPrgSize - 3] << 8);
            if (fixedResetVec >= 0x8000)
            {
                printf("ROM header fix: PRG %dK -> %dK, CHR found at PRG offset %d\n",
                       romSize / 1024, actualPrgSize / 1024, actualPrgSize);
                ROM = (BYTE *)nesFile;
                VROM = (BYTE *)(nesFile + actualPrgSize);
                NesHeader.byRomSize = actualPrgSize / 0x4000;
                return true;
            }
        }
    }

    ROM = (BYTE *)nesFile;
    nesFile += romSize;

    if (NesHeader.byVRomSize > 0)
    {
        VROM = (BYTE *)nesFile;
        nesFile += vromSize;
    }

    return true;
}

void InfoNES_ReleaseRom()
{
    ROM = nullptr;
    VROM = nullptr;
    if (IsNSF)
    {
        /* On a track-change reset (resetGame), keep NSF state alive so
           the current track number is preserved across the re-init. */
        if (!resetGame)
            nsfRelease();
        return;
    }
#if PICO_RP2350
    if (IsFDS)
    {
        fdsRelease();
        IsFDS = false;
        g_settings_visibility_nes[MOPT_FDS_DISK_SWAP] = 0;
        menuSetFdsHooks(nullptr);
    }
#endif
}

void InfoNES_SoundInit()
{
}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
    printf("InfoNES_SoundOpen: samples_per_sync=%d, sample_rate=%d\n", samples_per_sync, sample_rate);
    return 0;
}

void InfoNES_SoundClose()
{
}

int __not_in_flash_func(InfoNES_GetSoundBufferSize)()
{
    // Prefer early return to avoid duplicated branches.
#if EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio)
    {
        return audio_i2s_get_freebuffer_size();
    }
#endif

#if HSTX
    // Compute free HDMI Data Island audio packet capacity and convert to samples.
    int level = hstx_di_queue_get_level();
    int free_packets = HSTX_AUDIO_DI_HIGH_WATERMARK - level;
    if (free_packets <= 0)
        return 0;
    // Each DI packet carries 4 audio samples; use shift for fast multiply.
    return free_packets << 2;
#elif USE_ILI9341
    return 1024;
#else
    // Non-HSTX path: return available ring buffer capacity directly.
    return dvi_->getAudioRingBuffer().getFullWritableSize();
#endif
}



static inline int16_t apply_dvi_gain_i32(int x)
{
    int64_t v = (int64_t)x * (int64_t)g_dvi_audio_gain_q8; // Q8 scale
    v >>= 8;
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    return (int16_t)v;
}

static inline int16_t apply_record_gain_i32(int x)
{
    int64_t v = (int64_t)x * (int64_t)g_record_gain_q8; // Q8 scale
    v >>= 8;
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    return (int16_t)v;
}

static inline void set_dviaudio_gain_q8(int q8)
{
    if (q8 < 0) q8 = 0;
    if (q8 > 1024) q8 = 1024; // up to 4.0x
    g_dvi_audio_gain_q8 = q8;
}
static inline void recordSampleToSoundRecorder(int l, int r)
{
#if PICO_RP2350
    if (SoundRecorder::isRecording())
    {
        // int16_t cl = (l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        // int16_t cr = (r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
        int16_t cl = apply_record_gain_i32(l);
        int16_t cr = apply_record_gain_i32(r);
        int16_t stereo[2] = {cl, cr};
        SoundRecorder::recordFrame(stereo, 2);
    }
#endif
}

#if USE_ILI9341
#if EXT_AUDIO_IS_ENABLED
static uint32_t s_audio_batch[1024];
static int s_audio_batch_len = 0;
#endif

void __not_in_flash_func(InfoNES_SoundOutput)(int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3, BYTE *wave4, BYTE *wave5, BYTE *wave6)
{
    // DC blocker state. Larger shift = slower response / more low-frequency
    // preserved; smaller shift = stronger DC removal. 10 ≈ 20 Hz corner.
    static int32_t dc_l = 0;
    static int32_t dc_r = 0;
    constexpr int DC_FILTER_SHIFT = 10;

#if EXT_AUDIO_IS_ENABLED
    for (int i = 0; i < samples; ++i)
    {
        int w1 = wave1[i];
        int w2 = wave2[i];
        int w3 = wave3[i];
        int w4 = wave4[i];
        int w5 = wave5[i];
        int w6 = wave6 ? wave6[i] : 0;

        int raw_l = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;
        int raw_r = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;

        dc_l += (raw_l - dc_l) >> DC_FILTER_SHIFT;
        dc_r += (raw_r - dc_r) >> DC_FILTER_SHIFT;

        int out_l = (raw_l - dc_l) * 2;
        int out_r = (raw_r - dc_r) * 2;
        if (persistent_volume_level < 5)
        {
            out_l = (out_l * (int)persistent_volume_level) / 5;
            out_r = (out_r * (int)persistent_volume_level) / 5;
        }
#if PICO_RP2350
        recordSampleToSoundRecorder(out_l, out_r);
#endif
        if (s_audio_batch_len < 1024)
        {
            s_audio_batch[s_audio_batch_len++] = ((uint32_t)(uint16_t)(int16_t)out_l << 16) | (uint16_t)(int16_t)out_r;
        }
#if ENABLE_VU_METER
        if (settings.flags.enableVUMeter)
        {
            addSampleToVUMeter(out_l);
        }
#endif
    }

    if (s_audio_batch_len >= 512)
    {
        EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_batch, s_audio_batch_len);
        s_audio_batch_len = 0;
    }
#endif
    return;
#elif !HSTX
#if EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio)
    {
        for (int i = 0; i < samples; ++i)
        {
            int w1 = wave1[i];
            int w2 = wave2[i];
            int w3 = wave3[i];
            int w4 = wave4[i];
            int w5 = wave5[i];
            int w6 = wave6 ? wave6[i] : 0;

            int raw_l = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;
            int raw_r = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;

            dc_l += (raw_l - dc_l) >> DC_FILTER_SHIFT;
            dc_r += (raw_r - dc_r) >> DC_FILTER_SHIFT;

            int out_l = (raw_l - dc_l) * 2;
            int out_r = (raw_r - dc_r) * 2;
#if PICO_RP2350
            recordSampleToSoundRecorder(out_l, out_r);
#endif
            EXT_AUDIO_ENQUEUE_SAMPLE((int16_t)out_l, (int16_t)out_r);
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter)
            {
                addSampleToVUMeter(out_l);
            }
#endif
        }
        return;
    }
#endif
    while (samples)
    {
        auto &ring = dvi_->getAudioRingBuffer();
        auto n = std::min<int>(samples, ring.getWritableSize());
        if (!n)
        {
            return;
        }

        auto p = ring.getWritePointer();

        int ct = n;
        while (ct--)
        {
            int w1 = *wave1++;
            int w2 = *wave2++;
            int w3 = *wave3++;
            int w4 = *wave4++;
            int w5 = *wave5++;
            int w6 = wave6 ? *wave6++ : 0;

            int raw_l = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;
            int raw_r = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;

            dc_l += (raw_l - dc_l) >> DC_FILTER_SHIFT;
            dc_r += (raw_r - dc_r) >> DC_FILTER_SHIFT;

            int out_l = (raw_l - dc_l) * 2;
            int out_r = (raw_r - dc_r) * 2;
#if PICO_RP2350
            recordSampleToSoundRecorder(out_l, out_r);
#endif
            int l = apply_dvi_gain_i32(out_l);
            int r = apply_dvi_gain_i32(out_r);
            *p++ = {static_cast<short>(l), static_cast<short>(r)};
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter)
            {
                addSampleToVUMeter(out_l);
            }
#endif
        }

        ring.advanceWritePointer(n);
        samples -= n;
    }
#else
#if EXT_AUDIO_IS_ENABLED
    bool audioJackConnected = Frens::isHeadPhoneJackConnected();
#endif
    for (int i = 0; i < samples; ++i)
    {
        int w1 = wave1[i];
        int w2 = wave2[i];
        int w3 = wave3[i];
        int w4 = wave4[i];
        int w5 = wave5[i];
          /* w6: expansion audio
                - VRC6 (Konami Mapper 24)
                - Famicom Disk System (Mapper 20)
                - Sunsoft 5B (Mapper 69)
                - null when no expansion cart is loaded. */
        int w6 = wave6 ? wave6[i] : 0;

        int raw_l = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;
        int raw_r = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 51 + w5 * 80 + w6 * 18;

        dc_l += (raw_l - dc_l) >> DC_FILTER_SHIFT;
        dc_r += (raw_r - dc_r) >> DC_FILTER_SHIFT;

        int out_l = (raw_l - dc_l) * 2;
        int out_r = (raw_r - dc_r) * 2;

    #if PICO_RP2350
        recordSampleToSoundRecorder(out_l, out_r);
    #endif
    #if ENABLE_VU_METER
        if (settings.flags.enableVUMeter)
        {
            addSampleToVUMeter(out_l);
        }
    #endif

    #if USE_ILI9341
        #if EXT_AUDIO_IS_ENABLED
            EXT_AUDIO_ENQUEUE_SAMPLE((int16_t)out_l, (int16_t)out_r);
        #endif
            continue;
    #elif EXT_AUDIO_IS_ENABLED
        if (settings.flags.useExtAudio || audioJackConnected)
        {
            EXT_AUDIO_ENQUEUE_SAMPLE((int16_t)out_l, (int16_t)out_r);
            continue;
        }
    #endif

        int gl = apply_dvi_gain_i32(out_l);
        int gr = apply_dvi_gain_i32(out_r);
        hstx_push_audio_sample(gl, gr);
    }
#endif
}

extern WORD PC;

// Region-aware frame pacing.
//   NTSC: forwards to Frens::PaceFrames60fps (preserves the existing HSTX
//         vsync-wait and non-HSTX vsync/line-buffer behavior verbatim).
//   PAL : 50 Hz pacing via sleep_until. The HDMI/DVI panel still scans at
//         60 Hz, so 1 in every 6 displayed frames will be a duplicate; the
//         emulator itself runs at correct PAL speed. Works on HSTX (replaces
//         hstx_paceFrame's 60 Hz vsync wait) and on non-HSTX framebuffer
//         mode (replaces the vsync busy-wait). PAL is rejected upstream
//         when no framebuffer is available — see fallback at the call site.
static void paceFrame(bool init)
{
    if (persistent_turbo_mode)
    {
        static absolute_time_t next_turbo_time;
        static bool turbo_inited = false;
        if (init || !turbo_inited)
        {
            next_turbo_time = make_timeout_time_us(8333);
            turbo_inited = true;
        }
        if (time_reached(next_turbo_time))
        {
            next_turbo_time = make_timeout_time_us(8333);
        }
        else
        {
            sleep_until(next_turbo_time);
            next_turbo_time = delayed_by_us(next_turbo_time, 8333);
        }
        return;
    }

    if (!InfoNES_IsPal())
    {
        Frens::PaceFrames60fps(init);
        return;
    }

    static absolute_time_t next_frame_time;
    static bool initialized = false;
    if (init || !initialized)
    {
        next_frame_time = make_timeout_time_us(0);
        initialized = true;
    }

    // Slack-aware: if we overran the target by more than one frame (e.g.
    // returning from the menu after an idle pause), snap forward instead of
    // bursting through the backlog. Mirrors hstx_paceFrame's resync logic.
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, next_frame_time) <= -20000)
    {
        next_frame_time = now;
    }

    sleep_until(next_frame_time);
    next_frame_time = delayed_by_us(next_frame_time, 20000); // 1/50s = 20000us
#if DOUBLEFRAMEBUFFER
    Frens::swapFrameBuffers();
#endif
}

int InfoNES_LoadFrame()
{
#if USE_ILI9341
    /* Waking from standby: the game was started fresh, now put it back where
     * it was. Done at the first frame boundary, the same place the pause menu
     * loads a state from, with the blitter parked like it does. */
    if (s_resume_load_pending)
    {
        s_resume_load_pending = false;
        ili9341_blitter_stop();
        const bool ok = nes_load_state(romName, DMG_RESUME_SLOT);
        printf("[resume] state %s\n", ok ? "restored" : "missing, starting fresh");
        dmg_resume_consume();
        ili9341_blitter_start();
        ili9341_set_scanlines(persistent_scanlines_enabled);
        paceFrame(true);
    }

    /* Power switch OFF (or battery cutoff): save at this frame boundary and
     * sleep. Never returns -- the wake reboots into the code above. */
    if (s_request_power_off)
    {
        s_request_power_off = false;
        ili9341_blitter_stop();
        dmg_power_off_with_resume(&s_nes_desc, (dmg_power_reason_t)s_power_reason);
    }
#endif

    if (s_request_pause_menu)
    {
        s_request_pause_menu = false;
        ili9341_blitter_stop();
        ingame_action_e action = ingame_menu(romName);
        if (action == INGAME_ACTION_RESET)
        {
            resetGame = true;
            return -1;
        }
        else if (action == INGAME_ACTION_EXIT)
        {
            saveNVRAM();
            return -1;
        }
        else
        {
            ili9341_blitter_start();
            ili9341_set_scanlines(persistent_scanlines_enabled);
            paceFrame(true);
        }
    }

    if (s_request_exit_game)
    {
        s_request_exit_game = false;
        saveNVRAM();
        return -1;
    }

#if USE_ILI9341
#if EXT_AUDIO_IS_ENABLED
    if (s_audio_batch_len > 0)
    {
        EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_batch, s_audio_batch_len);
        s_audio_batch_len = 0;
    }
#endif
    ili9341_submit_frame();
#endif
    paceFrame(false);
    //Frens::waitForVSync();
    Frens::pollHeadPhoneJack();
    EXT_AUDIO_POLL_HEADPHONE();

    /* NSF: update VU meter levels once per frame */
    if (IsNSF)
    {
        nsfUpdateVuLevels();
        /* Check for auto-advance (silence detection / max duration) */
        if (nsfUpdatePlayback())
            resetGame = true;
    }
#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if USE_ILI9341
        0;
#elif !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    long onOff = hw_divider_s32_quotient_inlined(count, 60) & 1;
    Frens::blinkLed(onOff);
#if NES_PIN_CLK != -1
    nespad_read_finish(); // Sets global nespad_state var
#endif
    zapperPoll();    // Latch "Zapper present" on port 2 (+ optional debug trace)
    zapperMeasureFrame(); // Display-lag measurement, compiled out by default
    tuh_task();
    // Frame rate calculation
    if (settings.flags.displayFrameRate)
    {
        // calculate fps and round to nearest value (instead of truncating/floor)
        uint32_t tick_us = Frens::time_us() - start_tick_us;
        fps = (1000000 - 1) / tick_us + 1;
        start_tick_us = Frens::time_us();
    }

#if !HSTX
#else
    // hstx_waitForVSync();
#endif
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    // Poll Wii pad once per frame (function called once per rendered frame)
    wiipad_raw_cached = wiipad_read();
#endif
#if ENABLE_VU_METER
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        FrensSettings::savesettings();
        // printf("VU Meter %s\n", settings.flags.enableVUMeter ? "enabled" : "disabled");
        turnOffAllLeds();
    }
#endif
   
    if (showSettings && !IsNSF)
    {
        showSettings = false;
        int rval = showSettingsMenu(true);
        if (rval == 3)
        {
            reset = true;
            if (isAutoSaveStateConfigured() ){
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::SAVE_AND_EXIT;
            }
        }
        if ( rval == 4) {
            loadSaveStateMenu = true;
            quickSaveAction = SaveStateTypes::NONE;
           
        }
        if (rval == 5) {
           resetGame = true;
        }
    }
    if (loadSaveStateMenu && !IsNSF) {
        if (quickSaveAction == SaveStateTypes::LOAD_AND_START) {
            if (framesbeforeAutoStateIsLoaded > 0) {
                --framesbeforeAutoStateIsLoaded;  // let the emulator run for a few frames before loading state
            }   
        }  else {
            framesbeforeAutoStateIsLoaded = 0;
        } 
        if (framesbeforeAutoStateIsLoaded == 0) {
           
            char msg[24];
            snprintf(msg, sizeof(msg), "Mapper %03d CRC %08X", MapperNo, Frens::getCrcOfLoadedRom());
            if ( showSaveStateMenu(Emulator_SaveState, Emulator_LoadState, msg, quickSaveAction) == false ) {
                reset = true;
            };
            loadSaveStateMenu = false;
        }
    }

    return count;
}

namespace
{
#if USE_ILI9341
    WORD *currentLineBuffer_{nullptr};
    WORD *currentLineBuf{nullptr};
    static WORD s_nes_workline[320];
#elif !HSTX
    dvi::DVI::LineBuffer *currentLineBuffer_{};
    WORD *currentLineBuf{nullptr};
#else
    WORD *currentLineBuffer_{nullptr};
#endif
}
#if !HSTX && !USE_ILI9341
void __not_in_flash_func(drawWorkMeterUnit)(int timing,
                                            [[maybe_unused]] int span,
                                            uint32_t tag)
{
    if (timing >= 0 && timing < 640)
    {
        auto p = currentLineBuffer_->data();
        p[timing] = tag; // tag = color
    }
}

void __not_in_flash_func(drawWorkMeter)(int line)
{
    if (!currentLineBuffer_)
    {
        return;
    }

    memset(currentLineBuffer_->data(), 0, 64);
    memset(&currentLineBuffer_->data()[320 - 32], 0, 64);
    (*currentLineBuffer_)[160] = 0;
    if (line == 4)
    {
        for (int i = 1; i < 10; ++i)
        {
            (*currentLineBuffer_)[16 * i] = 31;
        }
    }

    constexpr uint32_t clocksPerLine = 800 * 10;
    constexpr uint32_t meterScale = 160 * 65536 / (clocksPerLine * 2);
    util::WorkMeterEnum(meterScale, 1, drawWorkMeterUnit);
    //    util::WorkMeterEnum(160, clocksPerLine * 2, drawWorkMeterUnit);
}
#endif

/*-------------------------------------------------------------------*/
/*  NSF display helper: draw a text string on a scanline             */
/*-------------------------------------------------------------------*/
static void nsfDrawText(WORD *buf, int x, int line, const char *text, int textRow, WORD fgc, WORD bgc)
{
    x+=2; // 2 pixel padding on the left
    for (int i = 0; text[i] != '\0'; i++)
    {
        char fontSlice = getcharslicefrom8x8font(text[i], textRow);
        for (int bit = 0; bit < 8; bit++)
        {
            buf[x + i * 8 + bit] = (fontSlice & 1) ? fgc : bgc;
            fontSlice >>= 1;
        }
    }
}

/*-------------------------------------------------------------------*/
/*  NSF display: render VU meter UI on a scanline                    */
/*                                                                   */
/*  Layout (320 pixels wide, lines 4-235):                           */
/*   Lines  20-27:  Song name                                        */
/*   Lines  36-43:  Artist name                                      */
/*   Lines  52-59:  Copyright                                        */
/*   Lines  72-79:  Track N / Total                                  */
/*   Lines  96-103: "Pulse 1" bar label                              */
/*   Lines 104-111: Pulse 1 VU bar                                   */
/*   Lines 116-123: "Pulse 2" bar label                              */
/*   Lines 124-131: Pulse 2 VU bar                                   */
/*   Lines 136-143: "Triangle" bar label                             */
/*   Lines 144-151: Triangle VU bar                                  */
/*   Lines 156-163: "Noise" bar label                                */
/*   Lines 164-171: Noise VU bar                                     */
/*   Lines 176-183: "DPCM" bar label                                 */
/*   Lines 184-191: DPCM VU bar                                      */
/*-------------------------------------------------------------------*/
static void nsfRenderLine(WORD *buf, int line)
{
    /* NES palette indices for colors */
    WORD bgc = NesPalette[0x0F];   /* Black */
    WORD fgc = NesPalette[0x30];   /* White */

    /* Fill background */
    for (int i = 0; i < 320; i++)
        buf[i] = bgc;

    /* Channel bar colors (NES palette) */
    static const BYTE barColors[5] = { 0x16, 0x12, 0x1A, 0x14, 0x17 };
    static const char *chanNames[5] = { "Pulse 1", "Pulse 2", "Triangle", "Noise", "DPCM" };

    /* Song name (lines 20-27) */
    if (line >= 20 && line < 28)
    {
        int textRow = line - 20;
        nsfDrawText(buf, 32, line, NsfHeader.szSongName, textRow, fgc, bgc);
    }
    /* Artist (lines 36-43) */
    else if (line >= 36 && line < 44)
    {
        int textRow = line - 36;
        nsfDrawText(buf, 32, line, NsfHeader.szArtistName, textRow, NesPalette[0x21], bgc);
    }
    /* Copyright (lines 52-59) */
    else if (line >= 52 && line < 60)
    {
        int textRow = line - 52;
        nsfDrawText(buf, 32, line, NsfHeader.szCopyright, textRow, NesPalette[0x21], bgc);
    }
    /* Track info (lines 72-79) */
    else if (line >= 72 && line < 80)
    {
        char trackStr[40];
        /* Show elapsed time MM:SS and play/stop status */
        int totalSec = NsfFrameCounter / 60;
        int mm = totalSec / 60;
        int ss = totalSec % 60;
        snprintf(trackStr, sizeof(trackStr), "Track %d / %d  %d:%02d %s",
                 NsfCurrentTrack + 1, NsfHeader.byTotalSongs,
                 mm, ss, NsfIsPlaying ? ">" : "||");
        int textRow = line - 72;
        nsfDrawText(buf, 32, line, trackStr, textRow, NesPalette[0x30], bgc);
    }
    /* VU bars: 5 channels, each occupies 20 lines (8 label + 8 bar + 4 gap) */
    else if (line >= 96 && line < 196)
    {
        for (int ch = 0; ch < 5; ch++)
        {
            int labelStart = 96 + ch * 20;
            int barStart = labelStart + 8;

            /* Channel label */
            if (line >= labelStart && line < labelStart + 8)
            {
                int textRow = line - labelStart;
                nsfDrawText(buf, 32, line, chanNames[ch], textRow, NesPalette[barColors[ch]], bgc);
            }
            /* VU bar */
            else if (line >= barStart && line < barStart + 8)
            {
                /* Bar width proportional to VU level (0-255 → 0-240 pixels) */
                int barWidth = (NsfVuLevels[ch] * 240) / 255;
                WORD barColor = NesPalette[barColors[ch]];
                for (int x = 40; x < 40 + barWidth && x < 280; x++)
                    buf[x] = barColor;
            }
        }
    }
    /* Progress bar (lines 210-215) */
    else if (line >= 210 && line < 216)
    {
        BYTE progress = nsfGetProgress();
        int barWidth = (progress * 240) / 255;
        WORD borderColor = NesPalette[0x10]; /* Dark grey */
        WORD fillColor = NesPalette[0x30];   /* White */

        /* Draw border on top/bottom lines, fill on interior */
        if (line == 210 || line == 215)
        {
            for (int x = 39; x <= 280; x++)
                buf[x] = borderColor;
        }
        else
        {
            buf[39] = borderColor;
            buf[280] = borderColor;
            for (int x = 40; x < 40 + barWidth && x < 280; x++)
                buf[x] = fillColor;
        }
    }
}

void __not_in_flash_func(InfoNES_PreDrawLine)(int line)
{
#if USE_ILI9341
    (void)line;
    currentLineBuf = s_nes_workline;
    InfoNES_SetLineBuffer(currentLineBuf, 256);
#elif !HSTX

    WORD *buff;
// b.size --> 640
// printf("Pre Draw%d\n", b->size());
// WORD = 2 bytes
// b->size = 640
// printf("%d\n", b->size());
#if FRAMEBUFFERISPOSSIBLE
    if (Frens::isFrameBufferUsed())
    {
        currentLineBuf = &Frens::framebuffer[line * 320];
        InfoNES_SetLineBuffer(currentLineBuf + 32, 320);
    }
    else
    {
#endif
        util::WorkMeterMark(0xaaaa);
        auto b = dvi_->getLineBuffer();
        util::WorkMeterMark(0x5555);
        InfoNES_SetLineBuffer(b->data() + 32, b->size());
        currentLineBuffer_ = b;
#if FRAMEBUFFERISPOSSIBLE
    }
#endif
    //    (*b)[319] = line + dvi_->getFrameCounter();

#else
    currentLineBuffer_ = hstx_getlineFromFramebuffer(line);
    InfoNES_SetLineBuffer(currentLineBuffer_ + 32, 640);

#endif
}

void __not_in_flash_func(InfoNES_PostDrawLine)(int line)
{
#if !HSTX && !USE_ILI9341
#if !defined(NDEBUG)
    util::WorkMeterMark(0xffff);
    drawWorkMeter(line);
#endif
#endif

    /* NSF mode: overwrite scanline with VU meter UI */
    if (IsNSF)
    {
        WORD *buf =
#if USE_ILI9341
            currentLineBuf;
#elif !HSTX
            currentLineBuf == nullptr ? currentLineBuffer_->data() : currentLineBuf;
#else
            currentLineBuffer_;
#endif
        nsfRenderLine(buf, line);
    }

    // Display frame rate
    if (settings.flags.displayFrameRate && line >= 8 && line < 16)
    {
        char fpsString[16];
        int nchars = 0;
        fpsString[nchars++] = '0' + (fps / 10);
        fpsString[nchars++] = '0' + (fps % 10);
#if HSTX
        // Append the HSTX auto-resync count so display glitches are visible.
        fpsString[nchars++] = ' ';
        fpsString[nchars++] = 'R';
        int resync = get_video_output_resync_count();
        char digits[10];
        int nd = 0;
        do
        {
            digits[nd++] = '0' + (resync % 10);
            resync /= 10;
        } while (resync > 0 && nd < (int)sizeof(digits));
        while (nd > 0 && nchars < (int)sizeof(fpsString))
        {
            fpsString[nchars++] = digits[--nd];
        }
#endif
        WORD *fpsBuffer =
#if USE_ILI9341
            currentLineBuf + 40;
#elif !HSTX
            currentLineBuf == nullptr ? currentLineBuffer_->data() + 40 : currentLineBuf + 40;
#else
            currentLineBuffer_ + 40;
#endif
        WORD fgc = NesPalette[48];
        WORD bgc = NesPalette[15];

        int rowInChar = line % 8;
        for (auto i = 0; i < nchars; i++)
        {
            char fontSlice = getcharslicefrom8x8font(fpsString[i], rowInChar);
            for (auto bit = 0; bit < 8; bit++)
            {
                if (fontSlice & 1)
                {
                    *fpsBuffer++ = fgc;
                }
                else
                {
                    *fpsBuffer++ = bgc;
                }
                fontSlice >>= 1;
            }
        }
    }

#if ZAPPER_SUPPORTED && ZAPPER_DEBUG_OVERLAY
    // Zapper status readout, drawn on the character row below the frame rate
    // counter. 32 characters at 8 pixels each is exactly the 256-pixel NES
    // picture, which starts at offset 32 in the 320-wide line buffer.
    //
    // Note this puts bright pixels on every frame, which the gun's sensor sees -
    // including the all-black and single-white-box frames the games use to work
    // out where the gun is pointed. Diagnostics only, never while playing.
    if (line >= 16 && line < 24)
    {
        WORD *zapBuffer =
#if USE_ILI9341
            currentLineBuf + 32;
#elif !HSTX
            currentLineBuf == nullptr ? currentLineBuffer_->data() + 32 : currentLineBuf + 32;
#else
            currentLineBuffer_ + 32;
#endif
        WORD fgc = NesPalette[48];
        WORD bgc = NesPalette[15];
        int rowInChar = line % 8;
        bool pastEnd = false;
        for (int i = 0; i < ZAPPER_DBG_TEXT_LEN; i++)
        {
            if (zapperDbgText[i] == '\0')
                pastEnd = true; // blank the tail rather than render stale bytes
            char fontSlice = getcharslicefrom8x8font(pastEnd ? ' ' : zapperDbgText[i], rowInChar);
            for (auto bit = 0; bit < 8; bit++)
            {
                *zapBuffer++ = (fontSlice & 1) ? fgc : bgc;
                fontSlice >>= 1;
            }
        }
    }
#endif

#if ZAPPER_SUPPORTED && ZAPPER_MEASURE
    // Display-lag measurement takes over the picture entirely: flat black or a
    // single flat white frame, so the only thing the gun can see is the flash.
    {
        WORD *mbuf =
#if USE_ILI9341
            currentLineBuf;
#elif !HSTX
            currentLineBuf == nullptr ? currentLineBuffer_->data() : currentLineBuf;
#else
            currentLineBuffer_;
#endif
        WORD c = zapperMeasureIsWhite() ? NesPalette[48] : NesPalette[15];
        for (int x = 32; x < 32 + 256; x++)
            mbuf[x] = c;
        zapperMeasureLine(line);
    }
#endif

#if USE_ILI9341
    ili9341_stream_nes_line(line, currentLineBuf);
#elif !HSTX
#if FRAMEBUFFERISPOSSIBLE
    if (!Frens::isFrameBufferUsed())
    {
#endif
        assert(currentLineBuffer_);
        dvi_->setLineBuffer(line, currentLineBuffer_);
        currentLineBuffer_ = nullptr;
#if FRAMEBUFFERISPOSSIBLE
    }
#endif
#endif
}

bool loadAndReset()
{
    auto rom = romSelector_.getCurrentROM();
    if (!rom)
    {
        printf("ROM does not exists.\n");
        return false;
    }

    if (!parseROM(rom))
    {
        printf("NES file parse error.\n");
        return false;
    }
    if (loadNVRAM() == false)
    {
        return false;
    }

    if (InfoNES_Reset() < 0)
    {
        printf("NES reset error.\n");
        return false;
    }
    return true;
}

int InfoNES_Menu()
{
    // InfoNES_Main() のループで最初に呼ばれる
    return loadAndReset() ? 0 : -1;
    // return 0;
}

int main()
{
    char selectedRom[FF_MAX_LFN + 1];

    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;

    vreg_voltage voltage = VREG_VOLTAGE_1_30;
#if HSTX
    Frens::FlashParams *flashParams;
    // assign flashParams to point to flash location
    bool freqOverruled = false;
    flashParams = (Frens::FlashParams *)FLASHPARAM_ADDRESS;
    if ( Frens::validateFlashParams(*flashParams) ) {
        CPUFreqKHz = flashParams->cpuFreqKHz;
        voltage = flashParams->voltage;
        freqOverruled = true;
    }
#endif
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);
#if USE_ILI9341
    /* Decouple SPI peripheral clock from CPU clock (252 MHz):
     * Route clk_peri to PLL_USB at 160 MHz (VCO 960 MHz, postdiv 6*1 = 6)
     * so divisor 2 gives an exact 80 MHz SPI clock for ILI9341/ST7789. */
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
    pll_init(pll_usb, 1, 960 * MHZ, 6, 1);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    160 * MHZ, 160 * MHZ);
#endif
    init_ui_buttons();

    printf("==========================================================================================\n");
    printf("Pico-InfoNES+ %s\n", SWVERSION);
    printf("Build date: %s\n", __DATE__);
    printf("Build time: %s\n", __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
#if HSTX
    printf("HSTX freq: %d\n", clock_get_hz(clk_hstx) / 1000);
#endif
    {
        // Which flash part is fitted decides how far the board can be overclocked,
        // so log it: genuine Picos carry a 133 MHz Winbond, clones often a 104 MHz
        // part that cannot keep up with clk_sys/2 at emulator speeds.
        uint32_t jedec = Frens::storage_get_flash_jedec_id();
        printf("Flash chip: JEDEC %06x (%s), %d MB\n",
               jedec,
               Frens::storage_get_flash_manufacturer_name((jedec >> 16) & 0xff),
               (1 << (jedec & 0xff)) / (1024 * 1024));
        printf("Flash freq: %d kHz\n", Frens::getFlashClockHz() / 1000);
    }
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");
    printf("Starting up...\n");
#if PICO_RP2350
    printf("Mapper 5 and Mapper 85 are enabled\n");
#else
    printf("Mapper 5 and Mapper 85 are disabled\n");
#endif
    FrensSettings::initSettings(FrensSettings::emulators::NES);
    // Note:
    //     - When using framebuffer, AUDIOBUFFERSIZE must be increased to 1024
    //     - Top and bottom margins are reset to zero
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 4, 4, AUDIOBUFFERSIZE, false, true);

#if USE_ILI9341
    dmg_init_boot_mode();
    load_picones_config();
    set_backlight_brightness(persistent_brightness_level);
    set_sound_volume_level(persistent_volume_level);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_interleave(persistent_interleave);
    g_crop_sides = persistent_crop_sides;
    ili9341_set_crop_sides(persistent_crop_sides);
    ili9341_set_height_fill(persistent_height_fill);
    settings.flags.displayFrameRate = persistent_display_fps;
    selectedRom[0] = 0;

    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. */
    dmg_power_enable_resume(&s_nes_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_nes_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            /* The bootloader skipped its intro; play it here while the ROM
             * loads on this core. The state goes in at the first frame. */
            dmg_boot_intro_start(&s_nes_desc);
            if (nes_on_load_rom(&s_nes_desc, rec.rom_file)) {
                snprintf(selectedRom, sizeof(selectedRom), "%s", rec.rom_file);
                s_resume_load_pending = true;
                dmg_power_arm_menu_loaded(1000);
            } else {
                printf("[resume] ROM %s did not load: ROM selector\n", rec.rom_file);
                dmg_resume_consume();
            }
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_nes_desc)) dmg_boot_intro_finish();
        }
    }
#endif

    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);
    initzapper();       // Claims GPIO27/28 as pulled-up inputs (custom PCB only)
    zapperMeasureInit(); // Display-lag measurement, compiled out by default
    bool showSplash = true;
#if PICO_RP2350
    g_settings_visibility_nes[MOPT_AUTO_SWAP_FDS_DISK] = 1;
    g_settings_visibility_nes[MOPT_AUTO_INSERT_FDS_DISK_A] = 1;
#else
    g_settings_visibility_nes[MOPT_AUTO_SWAP_FDS_DISK] =   0;
    g_settings_visibility_nes[MOPT_AUTO_INSERT_FDS_DISK_A] = 0;
#endif
#if HSTX
    if ( Frens::isPsramEnabled() ) {
        g_settings_visibility_nes[MOPT_OVERCLOCK] = 1;
    } else {
        g_settings_visibility_nes[MOPT_OVERCLOCK] = 0;
    }
#else
    g_settings_visibility_nes[MOPT_OVERCLOCK] = 0;
#endif
    g_settings_visibility = g_settings_visibility_nes;
    g_available_screen_modes = g_available_screen_modes_nes;
    while (true)
    {
#if USE_ILI9341
        /* No-op unless a resume intro is playing: wait for it, free core 1. */
        dmg_boot_intro_finish();
#endif
#if EMBEDDED_NES_ROM
        ROM_FILE_ADDR = (uintptr_t)embedded_nes_rom;
        strcpy(selectedRom, "Embedded");
        isFatalError = false; // SD card failure is not fatal when ROM is embedded
        *ErrorMessage = 0;
        Frens::PaceFrames60fps(true);
#else
        if (strlen(selectedRom) == 0)
        {
#if USE_ILI9341
            rom_file_selector(selectedRom);
            printf("Playing selected ROM from Clean Light menu: %s\n", selectedRom);
#else
#if PICO_RP2350
            const char *romExtensions = ".nes .fds .nsf";
#else
            const char *romExtensions = ".nes .nsf";
#endif
            menu("Pico-InfoNES+", ErrorMessage, isFatalError, showSplash, romExtensions, selectedRom); // With no psram this never returns, but reboots upon selecting a game
            printf("Playing selected ROM from menu: %s\n", selectedRom);
#endif
        }
        // Lagrange Point
        //  0x743387FF = Lagrange Point (Japan) (Rev A)
        //  0x00F49381 = English translation of Lagrange Point (Japan) (Rev A)
        if ( (Frens::getCrcOfLoadedRom() == 0x743387FF || Frens::getCrcOfLoadedRom() == 0x00F49381) ) 
        { 
#if HSTX
            if ( !Frens::isPsramEnabled() ) 
            {
                // Lagrange Point  needs PSRAM for its memory requirements.
                strcpy(ErrorMessage, "Lagrange Point needs PSRAM to run.");
                selectedRom[0] = 0;
                continue;
            }
            if ( !settings.flags.overclock ) 
            {
                // Lagrange Point  needs overclocking to run.
                strcpy(ErrorMessage, "Set Overclock ON in settings menu.");
                selectedRom[0] = 0;
                continue;
            }
#else
            strcpy(ErrorMessage, "Cannot run this game with this config.");
            selectedRom[0] = 0;
            continue;
#endif
           
        }
#endif
        reset = resetGame = loadSaveStateMenu = false;
        //EXT_AUDIO_MUTE_INTERNAL_SPEAKER(settings.flags.fruitJamEnableInternalSpeaker == 0);
        EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
        *ErrorMessage = 0;
        if (!Frens::isPsramEnabled())
        {
            printf("Now playing: %s\n", selectedRom);
        }
       
        if (isAutoSaveStateConfigured() && !IsNSF)
        {
            char tmpPath[40];
            getAutoSaveStatePath(tmpPath, sizeof(tmpPath));
            printf("Auto-save is configured found for this ROM (%s)\n", tmpPath);
            if (Frens::fileExists(tmpPath) ) {
                printf("Auto-save state found for this ROM (%s)\n", tmpPath);
                printf("Loading auto-save state...\n");
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::LOAD_AND_START;
                framesbeforeAutoStateIsLoaded = 120; // wait 2 seconds before loading auto state
            } else {
                printf("No auto-save state found for this ROM.\n");
            }
        } else {
            printf("No auto-save configured for this ROM.\n");
        }
        do {
            resetGame = false;
#if PICO_RP2350
            if (fdsIsFdsFilename(romName))
            {
                romSelector_.initRaw(ROM_FILE_ADDR);
            }
            else
#endif
            if (checkNSFMagic(reinterpret_cast<const uint8_t *>(ROM_FILE_ADDR)))
            {
                romSelector_.initRaw(ROM_FILE_ADDR);
            }
            else if (!romSelector_.init(ROM_FILE_ADDR) ) {
                printf("[E] Not a NES ROM file at 0x%08x\n", (unsigned)ROM_FILE_ADDR);
                strcpy(ErrorMessage, "Not a NES ROM file.");
                selectedRom[0] = 0;
                break;
            }

            // isRomPal: 0 = NTSC, 1 = PAL, 2 = Dendy.
            int region = InfoNES_DetectRegion(ROM_FILE_ADDR, Frens::getCrcOfLoadedRom(), selectedRom);
            static const char *regionNames[] = { "NTSC", "PAL", "Dendy" };
            const char *regionName = regionNames[region & 3];

            // PAL/Dendy require a framebuffer-based video pipeline. In non-HSTX
            // line-streaming mode (RP2040, no framebuffer) the line-buffer
            // queue is hardware-locked to the DVI 60 Hz scanline rate, so
            // pacing the emulator at 50 Hz starves the queue (flicker) and
            // the audio ring buffer (silence). Force NTSC there.
#if !HSTX
            if (region != INFONES_REGION_NTSC && !Frens::isFrameBufferUsed())
            {
                printf("%s not supported in line-streaming mode (no framebuffer); running as NTSC.\n", regionName);
                region = INFONES_REGION_NTSC;
                regionName = regionNames[0];
            }
#endif
            printf("Region: %s\n", regionName);
            // After a non-PSRAM reboot the monitor needs time to sync with the
            // fresh HDMI signal.  Without a delay the FDS BIOS intro animation
            // plays while the display is still dark.  Only needed on the very
            // first launch (showSplash is true); resets keep the link up.
            // This also benefits RP2040/RP2350: .nsf files don't clip sound at the start, roms that 
            // start with sound also don't clip sound.
#if !USE_ILI9341
            if (showSplash && !Frens::isPsramEnabled())
            {
                showSplash = false;
                printf("Feeding blank frames for display sync...\n");
                menuPumpBlankFrames(180);
            }
#endif
            paceFrame(true); // reset pacing to avoid burst of frames if resetGame is true
#if USE_ILI9341
            ili9341_blitter_start();
            ili9341_set_scanlines(settings.flags.scanlineOn);
#endif
            /* Drop any half-finished SELECT+START hold or closed edge latch
             * left over from the previous run. Without this, the hold counter
             * survives a soft reset and a reset game could exit on the first
             * frame if both buttons happened to still be down. */
            dmg_combo_reset();
            dmg_power_set_game(selectedRom, romName);
            InfoNES_Main(region);
#if USE_ILI9341
            ili9341_blitter_stop();
#endif

        } while (resetGame);
        dmg_power_set_game(NULL, NULL);
#if !EMBEDDED_NES_ROM
        selectedRom[0] = 0;
#endif
        showSplash = false;
    }

    return 0;
}
