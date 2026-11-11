#include "pico8_menu.h"
#include "mk_st7789.h"
#include "p8_audio.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define P8_STATE_MAGIC 0x50385353u /* "P8SS" */

static bool s_fps_enabled = false;
static bool s_turbo_enabled = false;

/* Display wrapper functions for shared_menu */
static void p8_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    mk_ili9225_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

static void p8_draw_text(const char *text, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg) {
    mk_ili9225_text((char *)text, x, y, fg, bg);
}

static void p8_fill_screen(uint16_t color) {
    mk_ili9225_fill(color);
}

static bool is_p8_cart(const char *filename) {
    if (!filename) return false;
    size_t len = strlen(filename);
    if (len >= 3 && strcasecmp(filename + len - 3, ".p8") == 0) return true;
    if (len >= 7 && strcasecmp(filename + len - 7, ".p8.png") == 0) return true;
    if (len >= 4 && strcasecmp(filename + len - 4, ".png") == 0) return true;
    return false;
}

#ifdef __cplusplus
extern "C" {
#endif
bool pico8_internal_save_state(const char *path);
bool pico8_internal_load_state(const char *path);
#ifdef __cplusplus
}
#endif

static bool p8_save_state(const char *game_name, uint8_t slot) {
    char path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/p8", game_name, slot, path, sizeof(path));
    f_mkdir("0:/saves");
    f_mkdir("0:/saves/p8");
    return pico8_internal_save_state(path);
}

static bool p8_load_state(const char *game_name, uint8_t slot) {
    char path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/p8", game_name, slot, path, sizeof(path));
    return pico8_internal_load_state(path);
}

static bool p8_state_exists(const char *game_name, uint8_t slot) {
    char path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/p8", game_name, slot, path, sizeof(path));
    return dmg_state_file_present(path);
}

static void p8_apply_scale(dmg_scale_mode_t scale_mode) {
    mk_ili9225_set_scale_mode((ili9341_scale_mode_t)scale_mode);
}

static void p8_apply_effect(bool effect_on) {
    mk_ili9225_set_grid_enabled(effect_on);
}

static void p8_apply_speed(bool turbo_on) {
    s_turbo_enabled = turbo_on;
}

bool pico8_menu_get_turbo(void) {
    return s_turbo_enabled;
}

void pico8_menu_set_turbo(bool turbo_on) {
    p8_apply_speed(turbo_on);
}

static void p8_apply_volume(uint8_t volume) {
    p8_audio_set_volume(volume);
}

static void p8_apply_brightness(uint8_t brightness) {
    mk_st7789_set_brightness(brightness);
}

static void p8_reset_game(void) {
    /* Will be handled in main game loop */
}

static void p8_exit_game(void) {
    /* Exiting returns to ROM selector */
}

static void p8_save_config(void);

static void draw_image_submenu_row(const dmg_emulator_desc_t *desc, const dmg_palette_t *pal, uint8_t idx, bool selected) {
    (void)desc;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? pal->sel : pal->bg;
    uint16_t fg = selected ? pal->text_sel : pal->text;

    p8_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    char buf[48];
    if (idx == 0) {
        const char *scale_name = "Fit";
        if (g_dmg_config.scale_mode == DMG_SCALE_STRETCH) {
            scale_name = "Stretch";
        } else if (g_dmg_config.scale_mode == DMG_SCALE_INTEGER) {
            scale_name = "Integer";
        }
        snprintf(buf, sizeof(buf), "Scale:        < %s >", scale_name);
    } else if (idx == 1) {
        snprintf(buf, sizeof(buf), "Grid Effect:  < %s >", g_dmg_config.grid_effect ? "On" : "Off");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Back");
    }

    p8_draw_text(buf, wx + 12, y + 7, fg, bg);
}

static void pico8_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    if (dmg_is_gb_mode()) {
        p8_fill_rect(0, 0, 45, 240, 0x0000);
        p8_fill_rect(311, 0, 9, 240, 0x0000);
        p8_fill_rect(wx, 0, ww, 240, pal.bg);
    } else {
        p8_fill_screen(pal.bg);
    }

    dmg_draw_top_bar(desc, "Pico8", "IMAGE");
    dmg_draw_bottom_bar(desc, "[A] Toggle  [<-/->] Alterar", "[B] Voltar");
    dmg_draw_game_bar(desc, game_name);

    uint8_t cur = 0;
    for (uint8_t i = 0; i < 3; i++) {
        draw_image_submenu_row(desc, &pal, i, (i == cur));
    }

    dmg_wait_for_all_buttons_released();

    bool up, down, left, right, a, b, select, start;
    bool prev_up = false, prev_down = false, prev_left = false, prev_right = false, prev_a = false, prev_b = false;

    while (true) {
        watchdog_update();
        dmg_read_ui_buttons(&up, &down, &left, &right, &a, &b, &select, &start);

        bool edge_up    = up && !prev_up;
        bool edge_down  = down && !prev_down;
        bool edge_left  = left && !prev_left;
        bool edge_right = right && !prev_right;
        bool edge_a     = a && !prev_a;
        bool edge_b     = b && !prev_b;

        prev_up = up; prev_down = down; prev_left = left; prev_right = right;
        prev_a = a; prev_b = b;

        if (edge_b) {
            dmg_wait_for_all_buttons_released();
            break;
        }

        if (edge_up && cur > 0) {
            uint8_t old = cur;
            cur--;
            draw_image_submenu_row(desc, &pal, old, false);
            draw_image_submenu_row(desc, &pal, cur, true);
        } else if (edge_down && cur < 2) {
            uint8_t old = cur;
            cur++;
            draw_image_submenu_row(desc, &pal, old, false);
            draw_image_submenu_row(desc, &pal, cur, true);
        }

        if (cur == 0 && (edge_left || edge_right || edge_a)) {
            if (edge_left) {
                if (g_dmg_config.scale_mode == DMG_SCALE_FIT) g_dmg_config.scale_mode = DMG_SCALE_INTEGER;
                else if (g_dmg_config.scale_mode == DMG_SCALE_INTEGER) g_dmg_config.scale_mode = DMG_SCALE_STRETCH;
                else g_dmg_config.scale_mode = DMG_SCALE_FIT;
            } else {
                if (g_dmg_config.scale_mode == DMG_SCALE_FIT) g_dmg_config.scale_mode = DMG_SCALE_STRETCH;
                else if (g_dmg_config.scale_mode == DMG_SCALE_STRETCH) g_dmg_config.scale_mode = DMG_SCALE_INTEGER;
                else g_dmg_config.scale_mode = DMG_SCALE_FIT;
            }
            p8_apply_scale((dmg_scale_mode_t)g_dmg_config.scale_mode);
            draw_image_submenu_row(desc, &pal, 0, true);
            p8_save_config();
        } else if (cur == 1 && (edge_left || edge_right || edge_a)) {
            g_dmg_config.grid_effect = !g_dmg_config.grid_effect;
            p8_apply_effect(g_dmg_config.grid_effect != 0);
            draw_image_submenu_row(desc, &pal, 1, true);
            p8_save_config();
        } else if (cur == 2 && edge_a) {
            dmg_wait_for_all_buttons_released();
            break;
        }

        sleep_ms(20);
    }
}

static const char *s_p8_candidate_dirs[] = {
    "0:/roms/p8",
    "0:/roms/pico8",
    "0:/roms/PICO8",
    "0:/p8",
    "0:/PICO8",
    NULL
};

static char s_current_game_name[64] = "PICO-8";

void pico8_menu_set_current_game(const char *name) {
    if (!name) return;
    const char *p = strrchr(name, '/');
    if (!p) p = strrchr(name, '\\');
    const char *base = p ? p + 1 : name;
    snprintf(s_current_game_name, sizeof(s_current_game_name), "%s", base);
    /* Strip extension .p8 or .png (twice in case of .p8.png) */
    char *dot = strrchr(s_current_game_name, '.');
    if (dot) *dot = '\0';
    dot = strrchr(s_current_game_name, '.');
    if (dot) *dot = '\0';
}

static dmg_emulator_desc_t s_pico8_desc = {
    .app_title        = "Pico8",
    .default_rom_dir  = "0:/roms/p8",
    .candidate_dirs   = s_p8_candidate_dirs,
    .config_path      = "0:/saves/p8/pico8.cfg",
    .config_fallback  = "/saves/p8/pico8.cfg",
    .saves_dir        = "0:/saves/p8",
    .config_magic     = 0x50384346u, /* "P8CF" */
    .is_rgb555        = false,
    .display = {
        .fill_rect    = p8_fill_rect,
        .draw_text    = p8_draw_text,
        .flush_rows   = NULL,
        .fill_screen  = p8_fill_screen,
        .flush        = NULL,
        .stop_blitter = NULL,
        .start_blitter = NULL,
        .write_rect_565 = mk_ili9225_write_rect_565
    },
    .is_rom_file      = is_p8_cart,
    .on_load_rom      = NULL,
    .loading_status   = "Carregando cartucho PICO-8...",
    .save_state       = p8_save_state,
    .load_state       = p8_load_state,
    .apply_scale      = p8_apply_scale,
    .apply_effect     = p8_apply_effect,
    .apply_interleave = NULL,
    .apply_speed      = p8_apply_speed,
    .apply_volume     = p8_apply_volume,
    .apply_brightness = p8_apply_brightness,
    .reset_game       = p8_reset_game,
    .exit_game        = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu       = pico8_image_submenu,
    .save_config          = p8_save_config,
    .get_fps              = pico8_menu_get_fps,
    .apply_fps            = pico8_menu_set_fps,
    .state_exists         = p8_state_exists,
    .audio_free           = p8_audio_intro_free,
    .audio_push           = p8_audio_intro_push,
    .flush_saves          = NULL,
    .audio_restore        = p8_audio_intro_restore
};

static void p8_save_config(void) {
    dmg_config_save(&s_pico8_desc);
}

bool pico8_menu_get_fps(void) {
    return s_fps_enabled;
}

void pico8_menu_set_fps(bool fps_on) {
    s_fps_enabled = fps_on;
    p8_save_config();
}

void pico8_menu_init(void) {
    dmg_config_load(&s_pico8_desc);
    /* Standby by save state: the power switch saves and sleeps (dmg_power.h). */
    dmg_power_enable_resume(&s_pico8_desc);
    mk_ili9225_set_scale_mode((ili9341_scale_mode_t)g_dmg_config.scale_mode);
    mk_ili9225_set_grid_enabled(g_dmg_config.grid_effect != 0);
    p8_apply_speed(g_dmg_config.turbo_mode != 0);
    p8_audio_set_volume((uint8_t)g_dmg_config.volume_level);
    mk_st7789_set_brightness((uint8_t)g_dmg_config.brightness_level);
}

const char *pico8_menu_get_current_game(void) {
    return s_current_game_name;
}

/* The resume state is Lua state, so it can only go in once the cart is running:
 * hal::update_button_state() calls this on the first frame, the same context
 * the pause menu loads a state from. */
static bool s_resume_load_pending = false;

void pico8_menu_set_resume_pending(bool pending) {
    s_resume_load_pending = pending;
}

void pico8_menu_service_resume(void) {
    if (!s_resume_load_pending) return;
    s_resume_load_pending = false;
    const bool ok = p8_load_state(s_current_game_name, DMG_RESUME_SLOT);
    printf("[resume] state %s\n", ok ? "restored" : "missing, starting fresh");
    dmg_resume_consume();
    dmg_power_arm_menu_loaded(1000);
}

const dmg_emulator_desc_t *pico8_menu_get_desc(void) {
    return &s_pico8_desc;
}

dmg_action_t pico8_menu_open(const char *game_name) {
    if (!game_name || game_name[0] == '\0') {
        game_name = s_current_game_name;
    }
    p8_audio_pause();
    dmg_action_t act = dmg_ingame_menu(&s_pico8_desc, game_name);
    p8_audio_resume();
    return act;
}

bool pico8_select_rom(char *out_path, size_t max_len) {
    char bare_name[256];
    bare_name[0] = '\0';
    if (!dmg_rom_selector(&s_pico8_desc, bare_name, sizeof(bare_name))) {
        return false;
    }
    if (bare_name[0] == '\0') {
        return false;
    }
    const char *dir = dmg_get_current_rom_dir();
    if (dir == NULL || dir[0] == '\0') {
        dir = s_pico8_desc.default_rom_dir ? s_pico8_desc.default_rom_dir : "0:/roms/p8";
    }
    if (bare_name[0] == '/' || (bare_name[0] != '\0' && bare_name[1] == ':')) {
        snprintf(out_path, max_len, "%s", bare_name);
    } else {
        snprintf(out_path, max_len, "%s/%s", dir, bare_name);
    }
    out_path[max_len - 1] = '\0';
    return true;
}

void pico8_show_status(const char *msg, uint16_t color) {
    if (dmg_boot_intro_active()) return;   /* the resume intro owns the panel */
    mk_ili9225_fill(0x0000);
    if (!msg || msg[0] == '\0') return;

    uint16_t base_x = dmg_is_gb_mode() ? (dmg_get_window_x() + 8) : 10;
    mk_ili9225_text((char *)msg, base_x, 110, color, 0x0000);
}

void pico8_show_error(const char *title, const char *msg) {
    mk_ili9225_fill(0x0000);

    uint16_t base_x = dmg_is_gb_mode() ? (dmg_get_window_x() + 8) : 10;
    uint16_t max_chars = dmg_is_gb_mode() ? 30 : 36;

    /* Header */
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%s", title ? title : "=== PICO-8 ERRO ===");
    mk_ili9225_text(hdr, base_x, 24, 0xF800, 0x0000);

    /* Body with word wrap */
    if (msg && msg[0] != '\0') {
        uint16_t cur_y = 48;
        const char *p = msg;
        char line_buf[48];
        while (*p && cur_y <= 192) {
            size_t seg_len = 0;
            while (p[seg_len] && p[seg_len] != '\n' && seg_len < max_chars) {
                seg_len++;
            }
            if (p[seg_len] != '\n' && seg_len == max_chars && p[seg_len] != '\0') {
                size_t last_break = seg_len;
                while (last_break > 0 && p[last_break] != ' ' && p[last_break] != ':' && p[last_break] != '/') {
                    last_break--;
                }
                if (last_break > max_chars / 2) {
                    seg_len = (p[last_break] == ' ') ? last_break : (last_break + 1);
                }
            }
            size_t copy_len = seg_len < sizeof(line_buf) - 1 ? seg_len : sizeof(line_buf) - 1;
            memcpy(line_buf, p, copy_len);
            line_buf[copy_len] = '\0';
            mk_ili9225_text(line_buf, base_x, cur_y, 0xFFFF, 0x0000);
            cur_y += 16;
            p += seg_len;
            if (*p == '\n' || *p == ' ') p++;
        }
    }

    /* Footer */
    mk_ili9225_text((char *)"[A/B] Pressione para voltar", base_x, 212, 0x07FF, 0x0000);

    /* Wait for buttons to be released first */
    dmg_wait_for_all_buttons_released();

    /* Wait loop: exit on A, B, or START, or after 15 seconds */
    for (int i = 0; i < 300; i++) {
        watchdog_update();
        bool up, down, left, right, a, b, select, start;
        dmg_read_ui_buttons(&up, &down, &left, &right, &a, &b, &select, &start);
        if (a || b || start) {
            break;
        }
        sleep_ms(50);
    }
    dmg_wait_for_all_buttons_released();
}

