#ifndef DMG_MENU_TYPES_H
#define DMG_MENU_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmg_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DMG_UI_SCREEN_WIDTH            320u
#define DMG_UI_SCREEN_HEIGHT           240u
#define DMG_UI_ITEM_HEIGHT             22u
#define DMG_UI_ROM_LIST_START_Y        21u
#define DMG_UI_INGAME_LIST_START_Y     43u
#define DMG_UI_ROM_SELECTOR_PAGE_SIZE  9u
#define DMG_UI_INGAME_ITEM_COUNT       8u

/* =========================================================================
 * Clean Light UI Color Palettes
 * ========================================================================= */

/* RGB565 Palette (Default - 16-bit) */
#define DMG_RGB565_BG_COLOR       0xFFFFu  /* Clean White (#FFFFFF) */
#define DMG_RGB565_BAR_COLOR      0xE73Cu  /* Light Gray (#E0E0E0) */
#define DMG_RGB565_BORDER_COLOR   0x0000u  /* Black Border (#000000) */
#define DMG_RGB565_TEXT_COLOR     0x0000u  /* Black Text (#000000) */
#define DMG_RGB565_TEXT_BAR_COLOR 0x2104u  /* Charcoal Text (#202020) */
#define DMG_RGB565_SEL_COLOR      0xB800u  /* Selection Bar Red (#BF0000) */
/* Text drawn ON the selection bar.
 *
 * Its own colour, not DMG_RGB565_TEXT_COLOR. The bar is #BF0000, whose sRGB
 * relative luminance is 0.111: black text on it is a 3.2:1 contrast ratio and
 * white is 6.5:1. The bar colour was chosen by the user and the black text came
 * with the mint green it was designed against, so it has to move with it --
 * otherwise every selected row turns into something you have to squint at. */
#define DMG_RGB565_TEXT_SEL_COLOR 0xFFFFu  /* White on the selection bar */
#define DMG_RGB565_STATUS_OK      0x07E0u  /* Pure Green (#00FF00) */
#define DMG_RGB565_STATUS_ERR     0xF800u  /* Pure Red (#FF0000) */

/* RGB555 Palette (Used by PicoNES and PicoSMS - 15-bit) */
#define DMG_RGB555_BG_COLOR       0x7FFFu  /* Clean White (#FFFFFF) */
#define DMG_RGB555_BAR_COLOR      0x739Cu  /* Light Gray (#E0E0E0) */
#define DMG_RGB555_BORDER_COLOR   0x0000u  /* Black Border (#000000) */
#define DMG_RGB555_TEXT_COLOR     0x0000u  /* Black Text (#000000) */
#define DMG_RGB555_TEXT_BAR_COLOR 0x1082u  /* Charcoal Text (#202020) */
#define DMG_RGB555_SEL_COLOR      0x5C00u  /* Selection Bar Red (#BF0000) */
#define DMG_RGB555_TEXT_SEL_COLOR 0x7FFFu  /* White on the selection bar */
#define DMG_RGB555_STATUS_OK      0x03E0u  /* Pure Green (#00FF00) */
#define DMG_RGB555_STATUS_ERR     0x7C00u  /* Pure Red (#FF0000) */

typedef struct {
    uint16_t bg;
    uint16_t bar;
    uint16_t border;
    uint16_t text;
    uint16_t text_bar;
    uint16_t text_sel;  /* text colour to use when the row sits on .sel */
    uint16_t sel;
    uint16_t status_ok;
    uint16_t status_err;
} dmg_palette_t;

typedef enum {
    DMG_ACTION_RESUME = 0,
    DMG_ACTION_RESET,
    DMG_ACTION_EXIT
} dmg_action_t;

typedef enum {
    DMG_SCALE_FIT = 0,
    DMG_SCALE_STRETCH = 1,
    DMG_SCALE_INTEGER = 2
} dmg_scale_mode_t;

/* =========================================================================
 * Persistent Emulator Configuration Layout
 * ========================================================================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t scale_mode;       /* 0: Fit, 1: Stretch, 2: Integer */
    uint32_t grid_effect;      /* 0: None, 1: Grid / Scanlines */
    uint32_t interleave;       /* 0: Progressive, 1: Interleave */
    uint32_t turbo_mode;       /* 0: 1x, 1: 2x */
    uint32_t volume_level;     /* 0..5 (0: Mute/Off, 1..5: Levels) */
    uint32_t brightness_level; /* 1..5 */
    uint32_t save_slot;        /* 1..4 */
    uint32_t load_slot;        /* 1..4 */
    uint32_t extra_opt1;       /* Platform extra (e.g. crop_sides, palette_idx) */
    uint32_t extra_opt2;       /* Platform extra (e.g. height_fill, 6-button) */
} dmg_config_t;

/* Forward declaration */
struct dmg_emulator_desc;

/* =========================================================================
 * Emulator Descriptor Contract
 * ========================================================================= */
typedef struct dmg_emulator_desc {
    const char *app_title;           /* "PicoNES", "PicoGBC", "PicoDMG", "PicoSMS", "PicoGG", etc. */
    const char *default_rom_dir;     /* "0:/roms/nes", "0:/roms/gb", "0:/roms/gg", etc. */
    const char **candidate_dirs;     /* Null-terminated array of fallback paths */
    const char *config_path;         /* "0:/saves/nes/picones.cfg", etc. */
    const char *config_fallback;     /* "/saves/nes/picones.cfg" */
    const char *saves_dir;           /* "0:/saves/nes", etc. */
    uint32_t    config_magic;        /* e.g. 0x4E455343u ("NESC") */
    bool        is_rgb555;           /* true for RGB555 palette, false for RGB565 */

    /* Display Driver Adapter */
    dmg_display_t display;

    /* ROM File Validation Callback */
    bool (*is_rom_file)(const char *filename);

    /* ROM Flashing / Loading Progress (Optional) */
    bool (*on_load_rom)(const struct dmg_emulator_desc *desc, const char *filename);

    /* State Callbacks */
    bool (*save_state)(const char *game_name, uint8_t slot);
    bool (*load_state)(const char *game_name, uint8_t slot);

    /* Hardware / Emulation Control Callbacks */
    void (*apply_scale)(dmg_scale_mode_t scale_mode);
    void (*apply_effect)(bool effect_on);
    void (*apply_interleave)(bool interleave_on);
    void (*apply_speed)(bool turbo_on);
    void (*apply_volume)(uint8_t volume);
    void (*apply_brightness)(uint8_t brightness);
    void (*apply_filter)(bool shimmerless_on);
    void (*reset_game)(void);
    void (*exit_game)(void);

    /* Platform-specific custom submenu (e.g. Palette, Controllers, Region) */
    const char *custom_submenu_title;
    void (*custom_submenu)(const struct dmg_emulator_desc *desc, const char *game_name);

    /* Footer text on the loading screen, describing where the ROM is going.
     * NULL keeps the default "Writing flash cache...", which is what every
     * flash-backed emulator wants. Boards that load the ROM into PSRAM instead
     * set this so the screen does not claim to write flash it never touches.
     * Appended at the end so existing designated and positional initialisers
     * keep compiling. */
    const char *loading_status;

    /* ---------------------------------------------------------------------
     * Persistence and FPS hooks used by the shared combo layer
     * (shared_menu/dmg_combo.c).
     *
     * save_config() is the emulator's own "write my config now" entry point.
     * The combo layer cannot call dmg_config_save() directly: PicoNES keeps a
     * private struct and PicoWS has to sync its own statics into g_dmg_config
     * before the write, so routing every emulator through one function is what
     * keeps each combo change from losing one of those steps.
     *
     * get_fps()/apply_fps() exist because the FPS overlay pixels belong to the
     * display driver, not to the menu: the combo decides when to toggle, the
     * emulator decides what that looks like. Optional -- a NULL apply_fps
     * simply means the emulator has no overlay.
     * ------------------------------------------------------------------- */
    void (*save_config)(void);
    bool (*get_fps)(void);
    void (*apply_fps)(bool on);

    /* ---------------------------------------------------------------------
     * Is there a loadable save state for this game in this slot?
     *
     * Optional. Left NULL, the shared menu probes
     * <saves_dir>/<game>_<slot>.ss itself, which is where every emulator that
     * builds its path with dmg_build_state_path() writes -- eight of the nine.
     *
     * An emulator needs its own only where the file name does not follow that
     * convention: PicoGBC/PicoDMG name it after the ROM header rather than the
     * cartridge file, and both it and PicoNES also fall back to an unnumbered
     * legacy name for slot 1.
     *
     * What it returns has to match what load_state() will actually find. If it
     * is stricter, the Load State row greys out a slot that loads; if it is
     * looser, it offers one that fails. ------------------------------------- */
    bool (*state_exists)(const char *game_name, uint8_t slot);

    /* ---------------------------------------------------------------------
     * Standby by save state (dmg_power.c / dmg_resume.c / dmg_boot_intro.c).
     *
     * audio_free()/audio_push() let the resume intro play the bootloader's
     * chime through the emulator's own I2S ring (44.1 kHz stereo). Called from
     * core 1 while core 0 loads the game, so they must not touch anything
     * core 0 uses during a load. NULL = silent intro.
     * ------------------------------------------------------------------- */
    int  (*audio_free)(void);
    void (*audio_push)(int16_t left, int16_t right);

    /* Write the cartridge battery save (.sav / flash / EEPROM) to the SD now.
     * Called by dmg_power_off_with_resume() after the resume state, so a power
     * cycle before the wake does not lose it. NULL falls back to exit_game. */
    void (*flush_saves)(void);

    /* Called by dmg_boot_intro_finish() once the intro is over: put the audio
     * pipeline back exactly as the emulator runs it (sample rate, ring empty,
     * DMA idle), since the intro played its chime through it from core 1.
     * NULL = nothing to restore. */
    void (*audio_restore)(void);
} dmg_emulator_desc_t;

#ifdef __cplusplus
}
#endif

#endif /* DMG_MENU_TYPES_H */

