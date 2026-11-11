#include "dmg_combo.h"
#include "dmg_menu.h"

/* =========================================================================
 * In-game combo layer
 * =========================================================================
 *
 * See dmg_combo.h for the contract. What lives here is only the detection:
 * edge/hold bookkeeping, priority between overlapping combos, and masking the
 * buttons a combo consumed so it never reaches the game as a stray press.
 *
 * The state is file-static on purpose. An emulator's frame loop is the only
 * caller, so there is exactly one pad in flight and a static is both smaller
 * and one less thing to thread through nine call sites. It does mean the state
 * survives a game switch, hence dmg_combo_reset().
 */

static uint16_t s_exit_hold;        /* SELECT+START frames held so far */
static bool     s_last_sta_b;       /* START+B edge latch */
static bool     s_last_sta_a;       /* START+A edge latch */
static bool     s_last_sel_a;       /* SELECT+A edge latch */
static bool     s_volume_active;    /* last frame: SELECT held with UP or DOWN */

void dmg_combo_reset(void) {
    s_exit_hold = 0;
    s_last_sta_b = false;
    s_last_sta_a = false;
    s_last_sel_a = false;
    s_volume_active = false;
}

bool dmg_combo_volume_active(void) {
    return s_volume_active;
}

static void mask_all(dmg_pad_t *pad) {
    pad->up = pad->down = pad->left = pad->right = false;
    pad->a = pad->b = pad->select = pad->start = false;
}

void dmg_combo_frame(dmg_pad_t *pad,
                     const dmg_emulator_desc_t *desc,
                     dmg_combo_events_t *ev) {
    if (pad == NULL) return;

    if (ev != NULL) {
        ev->request_pause_menu = false;
        ev->request_exit_game  = false;
        ev->speed_toggled      = false;
        ev->fps_toggled        = false;
        ev->request_power_off  = false;
        ev->power_reason       = DMG_POWER_REASON_SWITCH;
    }

    /* Power switch. With standby-by-save-state the OFF becomes an event the
     * emulator acts on at its frame boundary, where a save state is
     * consistent. Without it (emulators that have not opted in), the old
     * awake standby runs right here. */
    if (dmg_power_resume_enabled() && ev != NULL) {
        if (dmg_power_switch_off_confirmed()) {
            ev->request_power_off = true;
            ev->power_reason = DMG_POWER_REASON_SWITCH;
            mask_all(pad);
            return;
        }
    } else if (dmg_power_poll(desc)) {
        mask_all(pad);
        return;
    }

    /* Battery: samples every 2 s and refreshes the low-battery overlay. At the
     * 3.3 V cutoff the game leaves through its normal exit (which writes the
     * battery save); the menu it lands in shows the warning and the standby.
     * With standby-by-save-state the game is saved and slept instead, so the
     * player picks up where the battery died. */
    if (dmg_battery_game_tick()) {
        if (ev != NULL) {
            if (dmg_power_resume_enabled()) {
                ev->request_power_off = true;
                ev->power_reason = DMG_POWER_REASON_BATTERY;
            } else {
                ev->request_exit_game = true;
            }
        }
        mask_all(pad);
        return;
    }

    /* ---------------------------------------------------------------------
     * 1. SELECT + START -> leave the game.
     *
     * Held, not edge triggered: this is the only combo that ends the session,
     * so it costs a brush of the fingers to trigger by accident. 60 frames is
     * ~1 s at 60 Hz.
     *
     * While the hold is counting, everything is masked as well. The game keeps
     * running underneath, so SELECT+START held for most of a second would
     * otherwise deliver a continuous SELECT+START to the emulated console --
     * a Pause on the Master System, a Menu press on the Lynx.
     * ------------------------------------------------------------------- */
    if (pad->select && pad->start) {
        s_exit_hold++;
        if (s_exit_hold >= DMG_COMBO_EXIT_HOLD_FRAMES) {
            s_exit_hold = 0;
            /* Reset the edge latches: the player is still holding both buttons
             * and the next game must not inherit a half-closed latch. */
            s_last_sta_b = false;
            s_last_sta_a = false;
            s_last_sel_a = false;
            if (ev != NULL) ev->request_exit_game = true;
            mask_all(pad);
        } else {
            mask_all(pad);
        }
        return;
    }
    s_exit_hold = 0;

    /* ---------------------------------------------------------------------
     * 2. START + B -> pause menu. Edge triggered so holding it does not
     *    reopen the menu on every frame once it is dismissed.
     * ------------------------------------------------------------------- */
    if (pad->start && pad->b) {
        if (!s_last_sta_b && ev != NULL) ev->request_pause_menu = true;
        s_last_sta_b = true;
        pad->a = pad->b = pad->select = pad->start = false;
    } else {
        s_last_sta_b = false;
    }

    /* ---------------------------------------------------------------------
     * 3. START + A -> FPS overlay.
     *
     * The emulator owns the overlay pixels, so it supplies apply_fps and
     * get_fps; the layer only decides when to call them.
     * ------------------------------------------------------------------- */
    if (pad->start && pad->a) {
        if (!s_last_sta_a) {
            s_last_sta_a = true;
            if (desc != NULL && desc->apply_fps != NULL) {
                bool on = (desc->get_fps != NULL) ? desc->get_fps() : false;
                on = !on;
                desc->apply_fps(on);
                if (desc->save_config != NULL) desc->save_config();
                if (ev != NULL) ev->fps_toggled = true;
            }
        }
        pad->a = pad->start = false;
    } else {
        s_last_sta_a = false;
    }

    /* ---------------------------------------------------------------------
     * 4. SELECT + A -> 1x / 2x.
     *
     * This writes g_dmg_config.turbo_mode and calls apply_speed, which is
     * exactly what the in-game menu's Speed row does, so the combo and the
     * menu can never disagree about the current speed. Persistence goes
     * through desc->save_config rather than dmg_config_save() because how an
     * emulator saves is its own business -- PicoNES writes a private struct,
     * PicoWS syncs its own statics into g_dmg_config first.
     * ------------------------------------------------------------------- */
    if (pad->select && pad->a) {
        if (!s_last_sel_a) {
            s_last_sel_a = true;
            g_dmg_config.turbo_mode = g_dmg_config.turbo_mode ? 0u : 1u;
            if (desc != NULL) {
                if (desc->apply_speed != NULL) {
                    desc->apply_speed(g_dmg_config.turbo_mode != 0u);
                }
                if (desc->save_config != NULL) desc->save_config();
            }
            if (ev != NULL) ev->speed_toggled = true;
        }
        pad->a = pad->select = false;
    } else {
        s_last_sel_a = false;
    }

    /* ---------------------------------------------------------------------
     * 5. SELECT + UP / DOWN -> volume. The step and the deferred /boot.txt
     *    write already live in dmg_process_volume_combo.
     * ------------------------------------------------------------------- */
    s_volume_active = (pad->select && (pad->up || pad->down));
    if (dmg_process_volume_combo(pad->select, pad->up, pad->down, desc)) {
        pad->select = pad->up = pad->down = false;
    }
}