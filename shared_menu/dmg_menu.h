#ifndef DMG_MENU_H
#define DMG_MENU_H

#include "dmg_menu_types.h"
#include "dmg_panel.h"
#include "dmg_power.h"
#include "dmg_scale.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Global active configuration state */
extern dmg_config_t g_dmg_config;

/* Game Boy DMG Bezel Window Geometry (GB_MODE=1) */
#define DMG_GB_WINDOW_X     45u
#define DMG_GB_WINDOW_W     266u
#define DMG_GB_WINDOW_H     240u

/* Global Mode Management */
void     dmg_init_boot_mode(void);
bool     dmg_is_gb_mode(void);
uint16_t dmg_get_window_x(void);
uint16_t dmg_get_window_w(void);

/* Hardware Controls (GPIO 2..9) */
void dmg_init_ui_buttons(void);
void dmg_read_ui_buttons(bool *up, bool *down, bool *left, bool *right,
                         bool *a, bool *b, bool *select, bool *start);
void dmg_wait_for_all_buttons_released(void);

/* Bootloader Handshake */
bool dmg_is_launched_from_bootloader(void);
void dmg_reboot_to_bootloader(void);

/* Persistent Configuration */
void dmg_config_load(const dmg_emulator_desc_t *desc);
void dmg_config_save(const dmg_emulator_desc_t *desc);

/* Writes the configuration through the emulator's own save entry point.
 *
 * dmg_config_save() serialises g_dmg_config, but an emulator keeps its settings
 * in its own variables and only mirrors them into g_dmg_config inside its save
 * function. Anything that persists settings must go through this, or it writes a
 * half-stale record -- the shared menu changing the volume would put the new
 * volume on disk alongside the old scale mode. Falls back to
 * dmg_config_save() for an emulator that never supplied save_config. */
void dmg_config_commit(const dmg_emulator_desc_t *desc);

/* Global Volume Management (0..5: 0=Mute, 5=Max) */
uint8_t  dmg_get_global_volume(void);
void     dmg_set_global_volume(uint8_t vol, const dmg_emulator_desc_t *desc);
void     dmg_step_global_volume(int delta, const dmg_emulator_desc_t *desc);
void     dmg_flush_global_volume(void);

/* Volume OSD / HUD Overlay */
bool     dmg_process_volume_combo(bool btn_select, bool btn_up, bool btn_down, const dmg_emulator_desc_t *desc);
void     dmg_volume_hud_trigger(uint8_t volume_level);
void     dmg_volume_hud_render(const dmg_emulator_desc_t *desc);
bool     dmg_volume_hud_is_active(void);
const uint16_t *dmg_volume_hud_get_row(uint16_t screen_row, int *out_x, int *out_w);

/* Low-battery overlay for gameplay (battery <= 20%, no charger). Separate from
 * the volume HUD on purpose: drivers key cleanup and forced resends on the volume
 * HUD's on/off edge, and this one can stay up for minutes.
 *
 * Rendered on core 0, read by the core 1 blitter. Rows dmg_batt_overlay_y0() ..
 * y0 + dmg_batt_overlay_h() - 1, RGB565 panel format, 0x0001 = transparent.
 * get_row() returns NULL outside those rows or while hidden. generation()
 * changes whenever the content or the visibility changes. */
bool            dmg_batt_overlay_visible(void);
const uint16_t *dmg_batt_overlay_get_row(uint16_t screen_y, int *out_x, int *out_w);
uint32_t        dmg_batt_overlay_generation(void);
uint16_t        dmg_batt_overlay_y0(void);
uint16_t        dmg_batt_overlay_h(void);

/* Driver helpers for the low-battery overlay (core 1).
 *
 * stamp_row(): writes the overlay's opaque pixels into one rendered row. buf
 *   holds screen x = buf_x0 .. buf_x0 + buf_w - 1; swap = byte-swap the pixels
 *   (drivers that keep rows big-endian for SPI).
 * row_hit(): the overlay covers this screen row right now.
 * row_salt(): mix into a line hash so the overlay rows are resent when it
 *   appears, changes or hides (0 outside those rows).
 * push_borders(): the part of the overlay outside the game image
 *   [gx0,gx1) x [gy0,gy1), which the game never rewrites. Call once per frame;
 *   push() gets a w*h block of native RGB565 pixels (not swapped). */
typedef void (*dmg_batt_push_fn)(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 const uint16_t *pixels);
void     dmg_batt_overlay_stamp_row(uint16_t screen_y, uint16_t *buf, int buf_x0, int buf_w, bool swap);
bool     dmg_batt_overlay_row_hit(uint16_t screen_y);
uint32_t dmg_batt_overlay_row_salt(uint16_t screen_y);
void     dmg_batt_overlay_push_borders(int gx0, int gx1, int gy0, int gy1, dmg_batt_push_fn push);
/* push_borders() would draw or clear something this frame */
bool     dmg_batt_overlay_borders_pending(void);

/* Queue-based drivers: mark_row() for every row the game emits (before any
 * unchanged-row skip); frame_rows() once per frame that emitted rows returns a
 * bitmask (bit i = row y0 + i) of overlay rows to queue as full-width black
 * rows with the overlay stamped. Rows the game emits in y0 .. y0+h-1 must mix
 * row_salt() into their hash and go out at full width. */
void     dmg_batt_overlay_mark_row(uint16_t row);
uint32_t dmg_batt_overlay_frame_rows(void);

/* Per-frame battery service for gameplay, called by dmg_combo_frame(). Samples
 * every 2 s and refreshes the overlay. Returns true once the 3.3 V cutoff is
 * confirmed: the game must exit, and the menu it returns to shows the warning
 * and enters the low-battery standby. */
bool            dmg_battery_game_tick(void);

/* Palette retrieval */
void dmg_get_palette(const dmg_emulator_desc_t *desc, dmg_palette_t *out_pal);

/* UI Drawing Primitives */
void dmg_draw_top_bar(const dmg_emulator_desc_t *desc, const char *title, const char *subtitle);
void dmg_draw_bottom_bar(const dmg_emulator_desc_t *desc, const char *left_hint, const char *right_hint);
void dmg_draw_game_bar(const dmg_emulator_desc_t *desc, const char *game_name);
void dmg_draw_status(const dmg_emulator_desc_t *desc, const char *msg, uint16_t color);

/* Current ROM directory */
const char *dmg_get_current_rom_dir(void);
void dmg_set_current_rom_dir(const char *dir);

/* Clean Light Loading / Progress Screen */
void dmg_draw_loading_screen(const dmg_emulator_desc_t *desc, const char *title, const char *game_name, uint32_t total_size);
void dmg_update_loading_progress(const dmg_emulator_desc_t *desc, uint32_t bytes_loaded, uint32_t total_size);

/* The single text-margin rule shared by every screen, in whole glyph columns.
 *
 * The margins are DMG_UI_TEXT_MARGIN (12 px) on the left, and whatever is left
 * over on the right after rounding the frame down to the 8 px glyph grid:
 *   full screen  ww=320 -> frame 296 px = 37 cols, left 12 / right 12
 *   GB_MODE=1    ww=266 -> frame 240 px = 30 cols, left 12 / right 14
 *
 * Exported so screens that cannot use the shared drawing helpers still measure
 * the same frame instead of hardcoding coordinates against a flat 320x240. */
uint16_t dmg_text_frame_w(uint16_t window_w);

/* Word-wrapped text inside a frame of frame_w pixels, at most max_lines lines.
 *
 * Wraps on spaces; a single word longer than the frame is hard-split rather than
 * allowed to overflow. When the text still does not fit in max_lines, the last
 * line is shortened and terminated with "..." so nothing is ever clipped by the
 * panel margin. Lines advance by line_pitch_px (the glyph is 8 px tall, so
 * anything >= 9 keeps adjacent lines from touching).
 *
 * x/y are the top-left of the first line; drawing is left-aligned to x. */
void dmg_draw_wrapped_text(const dmg_emulator_desc_t *desc, const char *text,
                           int16_t x, int16_t y,
                           uint16_t fg, uint16_t bg,
                           uint16_t frame_w, uint8_t line_pitch_px, uint8_t max_lines);

/* In-game Pause Menu: returns DMG_ACTION_RESUME, DMG_ACTION_RESET, or DMG_ACTION_EXIT */
dmg_action_t dmg_ingame_menu(const dmg_emulator_desc_t *desc, const char *game_name);

/* ROM File Selector: fills out_selected_rom with chosen filename */
bool dmg_rom_selector(const dmg_emulator_desc_t *desc, char *out_selected_rom, size_t max_len);

/* Helper to sanitize game name into safe filename: "Super Mario Bros" -> "Super_Mario_Bros" */
void dmg_sanitize_basename(const char *game_name, char *out, size_t out_len);

/* Minimum buffer for the `out` of dmg_build_state_path(): 96 bytes.
 *
 * NOT a style preference -- this is the size that a truncated path silently
 * breaks. The worst case is "0:/saves/a26/" (15) + a 63-char sanitised game
 * name + "_4.ss" (5) + NUL = 84, so 96 fits any directory this project uses.
 *
 * It was 64 in three emulators, and 64 is one byte too few:
 *
 *     "0:/saves/sms/" (13) + "Alex_Kidd_in_Miracle_World_(World)_(Sega_Ages)"
 *     (46) + "_1.ss" (5) = 64 chars + NUL
 *
 * snprintf kept 63 characters, dropped the last one, and the state file landed
 * on the card as `..._1.s`. Save and load used the same truncated buffer, so they
 * agreed with each other and the round trip worked -- which is exactly why it
 * went unnoticed until something asked for the same name with a correct buffer
 * and did not find it. */
#define DMG_STATE_PATH_MAX 96u

/* Slot of the standby-resume save state: written when the power switch goes
 * OFF in a game, read back on the way up. dmg_build_state_path() names it
 * "<game>_R.ss", which the 1..4 Load State row never probes. */
#define DMG_RESUME_SLOT 0u

/* Helper to generate state path: 0:/saves/<dir>/<game>_<slot>.ss */
void dmg_build_state_path(const char *saves_dir, const char *game_name, uint8_t slot, char *out, size_t out_len);

/* Is there a state file at this path that holds something loadable?
 *
 * A zero-length file does not count: there is one on the cards already, left
 * by a save that was interrupted. Exported because an emulator whose state
 * files are not named <game>_<slot>.ss has to answer the same question for its
 * own names -- see dmg_emulator_desc_t::state_exists. */
bool dmg_state_file_present(const char *path);

/* Can slot (1..4) of this game be loaded, i.e. does it have a state?
 *
 * Delegates to desc->state_exists when the emulator supplied one, otherwise
 * probes <saves_dir>/<game>_<slot>.ss, which is what dmg_build_state_path()
 * produces and therefore where every emulator that uses it writes. This is what
 * lets the Load State row grey out the empty slots and refuse to navigate onto
 * them, so an emulator with custom naming MUST supply the callback. */
bool dmg_state_slot_exists(const dmg_emulator_desc_t *desc, const char *game_name, uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* DMG_MENU_H */
