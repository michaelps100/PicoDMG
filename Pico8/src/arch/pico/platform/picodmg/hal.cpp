#include "video/palette.hpp"
#include <hal/hal.hpp>
#include <emu/emulator.hpp>
#include <ldo.hpp>
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "p8_audio.h"
#include "i2s.h"
#include "pico8_menu.h"
#include "mk_st7789.h"
#include <pico/time.h>

namespace hal {

hal::ButtonState update_button_state() {
    /* First frame after a standby wake: put the cart back where it was. */
    pico8_menu_service_resume();

    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, select = false, start = false;
    dmg_read_ui_buttons(&up, &down, &left, &right, &a, &b, &select, &start);

    /* Check SELECT + START 1-second hold combo to exit back to game selector */
    static uint64_t s_sel_start_press_time = 0;
    bool exit_requested = false;

    if (select && start) {
        if (s_sel_start_press_time == 0) {
            s_sel_start_press_time = time_us_64();
        } else if ((time_us_64() - s_sel_start_press_time) >= 1000000ULL) {
            /* Held for 1 second (~1000 ms) */
            exit_requested = true;
        }
    } else {
        s_sel_start_press_time = 0;
    }

    /* Process system combos (SELECT+UP/DOWN for volume HUD, SELECT+A for turbo, etc.) */
    dmg_pad_t combo_pad = {
        .up = up, .down = down, .left = left, .right = right,
        .a = a, .b = b, .select = select, .start = start
    };
    dmg_combo_events_t combo_ev;
    dmg_combo_frame(&combo_pad, pico8_menu_get_desc(), &combo_ev);

    /* Power switch OFF (or battery cutoff): save here -- the same context the
     * pause menu saves from -- and sleep. Never returns: the wake reboots. */
    if (combo_ev.request_power_off) {
        p8_audio_pause();
        dmg_power_off_with_resume(pico8_menu_get_desc(), (dmg_power_reason_t)combo_ev.power_reason);
    }

    if (combo_ev.request_exit_game) {
        exit_requested = true;
    }

    if (exit_requested) {
        s_sel_start_press_time = 0;
        dmg_wait_for_all_buttons_released();
        dmg_combo_reset();
        p8_audio_pause();
        p8_audio_reset();
        throw EmulatorExit();
    }

    /* START + B: Open in-game pause menu */
    static bool s_sta_b_edge = false;
    bool req_menu = combo_ev.request_pause_menu;
    if (start && b) {
        if (!s_sta_b_edge) {
            s_sta_b_edge = true;
            req_menu = true;
        }
    } else {
        s_sta_b_edge = false;
    }

    if (req_menu) {
        dmg_action_t act = pico8_menu_open("");
        if (act == DMG_ACTION_EXIT) {
            dmg_wait_for_all_buttons_released();
            dmg_combo_reset();
            p8_audio_pause();
            p8_audio_reset();
            throw EmulatorExit();
        } else if (act == DMG_ACTION_RESET) {
            dmg_wait_for_all_buttons_released();
            dmg_combo_reset();
            p8_audio_reset();
            throw EmulatorReset();
        }
        mk_ili9225_fill(0x0000);
        dmg_wait_for_all_buttons_released();
    }

    /* If SELECT and START are being held together, mask them so game doesn't receive stray inputs */
    if (select && start) {
        combo_pad.start = false;
        combo_pad.select = false;
        combo_pad.a = false;
        combo_pad.b = false;
    }

    /* PICO-8 button mapping:
     * 0: Left, 1: Right, 2: Up, 3: Down, 4: O (A), 5: X (B), 6: Pause (Start) */
    uint8_t held_key_mask = 0;
    if (combo_pad.left)  held_key_mask |= (1u << 0);
    if (combo_pad.right) held_key_mask |= (1u << 1);
    if (combo_pad.up)    held_key_mask |= (1u << 2);
    if (combo_pad.down)  held_key_mask |= (1u << 3);
    if (combo_pad.a)     held_key_mask |= (1u << 4);
    if (combo_pad.b)     held_key_mask |= (1u << 5);
    if (combo_pad.start) held_key_mask |= (1u << 6);

    /* PICO-8 btnp repeat logic:
     * - Fires on frame 0 (first press)
     * - Fires on frame init_delay (default 15 from 0x5F5C)
     * - Fires every rep_delay frames thereafter (default 4 from 0x5F5D)
     * - If init_delay == 255: no auto-repeat
     */
    static uint8_t s_hold_counters[8] = {0};
    uint8_t pressed_key_mask = 0;

    uint8_t init_delay = 15;
    uint8_t rep_delay = 4;
    auto mem_view = emu::emulator.memory().raw_view;
    if (mem_view.size() > 0x5F5D) {
        uint8_t id = mem_view[0x5F5C];
        uint8_t rd = mem_view[0x5F5D];
        if (id != 0) init_delay = id;
        if (rd != 0) rep_delay = rd;
    }

    for (int b = 0; b < 8; b++) {
        if (held_key_mask & (1u << b)) {
            if (s_hold_counters[b] == 0) {
                pressed_key_mask |= (1u << b);
                s_hold_counters[b] = 1;
            } else {
                if (s_hold_counters[b] < 254) s_hold_counters[b]++;
                if (init_delay != 255) {
                    if (s_hold_counters[b] == init_delay) {
                        pressed_key_mask |= (1u << b);
                    } else if (s_hold_counters[b] > init_delay && rep_delay > 0) {
                        if (((s_hold_counters[b] - init_delay) % rep_delay) == 0) {
                            pressed_key_mask |= (1u << b);
                        }
                    }
                }
            }
        } else {
            s_hold_counters[b] = 0;
        }
    }

    /* Feed I2S audio with adaptive watermark cushion (prevents underflow clicks during slow frames) */
    int fps = emu::emulator.get_fps_target();
    bool turbo = pico8_menu_get_turbo();
    int base_samples = (fps == 60) ? 368 : 735;
    if (turbo) {
        base_samples /= 2;
    }

    size_t avail = i2s_get_available_samples();
    const size_t target_cushion = 1470; /* ~33ms safety margin */
    int to_gen = base_samples;
    if (avail < target_cushion) {
        int diff = (int)(target_cushion - avail);
        if (diff > 368) diff = 368;
        to_gen += diff;
    } else if (avail > target_cushion + 735) {
        int diff = (int)(avail - (target_cushion + 735));
        if (diff > 368) diff = 368;
        to_gen -= diff;
    }
    if (to_gen < 184) to_gen = 184;
    p8_audio_update_samples(to_gen);

    return {
        .held_key_mask = held_key_mask,
        .pressed_key_mask = pressed_key_mask
    };
}

void load_rgb_palette(std::span<std::uint32_t, 32> new_palette) {
    (void)new_palette;
}

std::span<const std::uint32_t, 32> get_default_palette() {
    return video::pico8_palette_rgb8;
}

} // namespace hal
