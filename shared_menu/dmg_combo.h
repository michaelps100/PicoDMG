#ifndef DMG_COMBO_H
#define DMG_COMBO_H

#include <stdint.h>
#include <stdbool.h>
#include "dmg_menu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * In-game combo layer shared by every emulator
 * =========================================================================
 *
 * The nine emulators each had their own copy of the SELECT+START exit, the
 * START+A FPS overlay, the START+B pause menu and the SELECT+A speed toggle.
 * The copies had drifted: four different `s_exit_hold_frames` types, two reset
 * policies, an edge predicate in PicoPCE that also fired when only one of the
 * two buttons was newly pressed, and six emulators with no SELECT+A at all.
 *
 * This module owns the detection. An emulator only forwards the raw pad and
 * acts on the events it gets back.
 *
 * Detection is edge or hold based on purpose:
 *   - a toggle must not re-trigger while held, so it is edge triggered;
 *   - an exit must not fire from a brush of the fingers, so it requires a hold.
 */

/* How long SELECT+START must stay down before the game exits, in frames.
 * 60 frames is ~1 s at 60 Hz, the value already validated on PicoGBC/PicoDMG,
 * PicoNGP and PicoWS. */
#ifndef DMG_COMBO_EXIT_HOLD_FRAMES
#define DMG_COMBO_EXIT_HOLD_FRAMES 60u
#endif

/* Physical pad state for one frame. All fields are `true` when pressed. */
typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;
    bool a;
    bool b;
    bool select;
    bool start;
} dmg_pad_t;

/* What the combos did this frame. */
typedef struct {
    bool request_pause_menu;  /* START + B */
    bool request_exit_game;   /* SELECT + START held long enough */
    bool speed_toggled;       /* SELECT + A (1x <-> 2x) */
    bool fps_toggled;         /* START + A */
    /* Power switch OFF (or the low-battery cutoff) on an emulator that opted
     * in to standby-by-save-state (dmg_power_enable_resume). Act on it at the
     * next frame boundary: stop the blitter and call
     * dmg_power_off_with_resume(desc, power_reason), which never returns.
     * Never raised for an emulator that did not opt in. */
    bool request_power_off;
    uint8_t power_reason;     /* dmg_power_reason_t */
} dmg_combo_events_t;

/* Clears the hold counters and the edge latches.
 *
 * Call this when a game starts. The state is file-static, so without it a
 * half-finished hold from the previous game would carry over: releasing and
 * re-pressing SELECT+START one frame after a game loads would exit
 * immediately, which looks exactly like the hold being ignored. */
void dmg_combo_reset(void);

/* Runs one frame of combo detection.
 *
 * `pad` is read and written: any button consumed by a combo is cleared before
 * returning, so forwarding `pad` to the emulated console is what keeps a combo
 * from reaching the game as a stray press. Callers that need to tell "the user
 * is holding SELECT" from "SELECT survived the combo pass" compare against
 * pad->select afterwards, which is false whenever a combo took it.
 *
 * `desc` may be NULL, in which case the speed toggle only updates
 * g_dmg_config and the emulators without descriptor callbacks simply do not
 * get one. `ev` may be NULL if only the pad masking is wanted.
 *
 * Priority when several combos match the same press, highest first:
 *   1. SELECT + START   leaving the game wins over everything
 *   2. START + B        the pause menu, so it cannot be swallowed by FPS/speed
 *   3. START + A        FPS overlay
 *   4. SELECT + A       1x / 2x
 *   5. SELECT + UP/DOWN volume (0..5)
 *
 * SELECT+START is held rather than edge triggered on purpose: it is the only
 * combo that destroys state, since it saves and leaves the game.
 */
void dmg_combo_frame(dmg_pad_t *pad,
                     const dmg_emulator_desc_t *desc,
                     dmg_combo_events_t *ev);

/* Whether the volume combo is currently driving the D-pad, i.e. whether
 * SELECT is held together with UP or DOWN. Kept exported because the volume
 * write to /boot.txt is deferred to the SELECT release and callers that mirror
 * SELECT somewhere else (PicoWS rotates the screen on SELECT release, PicoMD
 * maps SELECT onto a console button) need the same signal. */
bool dmg_combo_volume_active(void);

#ifdef __cplusplus
}
#endif

#endif /* DMG_COMBO_H */