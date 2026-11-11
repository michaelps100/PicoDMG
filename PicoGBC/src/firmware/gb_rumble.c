/*
 * gb_rumble.c - see gb_rumble.h.
 */
#include "gb_rumble.h"

#include <hardware/gpio.h>
#include <stdio.h>

#include "dmg_panel.h"

static uint8_t s_pin = 0;    /* 0 = no motor */
static bool s_claimed = false;

uint8_t gb_rumble_init(void)
{
    /* Re-read every call: the first happens before the SD card is mounted, when
     * the file cannot be opened, and the second is the one that sees RUMBLE=. */
    const uint8_t want = dmg_panel_get_rumble_pin();

    if (s_claimed) {
        if (want == s_pin) return s_pin;
        /* The configured pin changed between calls. Park the old one rather
         * than leaving it driven, then fall through and claim the new one. */
        gb_rumble_shutdown();
    }

    if (want == 0) {
        s_pin = 0;
        return 0;
    }

    /* RP2350 GPIO 0..29. Anything above is not a pin, and the button range is
     * already spoken for. */
    if (want > 29u) {
        printf("W RUMBLE=%u is not a valid GPIO; rumble disabled\n", (unsigned)want);
        s_pin = 0;
        return 0;
    }
    if (want <= 9u) {
        /* Refused rather than silently honoured: taking a button pin would
         * break the controls, and a console that boots with dead buttons is
         * worse than one with no rumble. */
        printf("W RUMBLE=%u collides with a button input; rumble disabled\n",
               (unsigned)want);
        s_pin = 0;
        return 0;
    }

    gpio_init(want);
    gpio_set_dir(want, GPIO_OUT);
    /* Parked before it is ever driven: a floating motor driver input would
     * otherwise let the motor twitch on power-up. */
    gpio_put(want, 0);

    s_pin = want;
    s_claimed = true;
    printf("I rumble motor on GPIO %u\n", (unsigned)want);
    return s_pin;
}

void gb_rumble_set(bool on)
{
    if (!s_claimed) return;

    /* MBC5 rumble is a plain on/off motor: there is no speed register on the
     * cartridge, and the enable bit is what the hardware wires straight to the
     * motor. PWM here would be an embellishment rather than an emulation. */
    gpio_put(s_pin, on ? 1 : 0);
}

bool gb_rumble_available(void)
{
    return s_claimed;
}

/* Park the motor without giving up the pin.
 *
 * This deliberately does NOT clear s_claimed. It is called on the way out of a
 * game, and the ROM selector then offers another one -- and gb_rumble_init()
 * only runs once, before the selector, so releasing the claim here left the
 * second cartridge of a session permanently unable to drive the motor. The
 * symptom was that the first rumble game worked and every later one was silent
 * until the console was reset.
 *
 * "Which pin do I own" does not depend on which cartridge is running, so parking
 * is all that is needed here. The pin stays configured and stays parked, which
 * is what the selector wants: no cartridge is running, so no rumble. */
void gb_rumble_shutdown(void)
{
    if (s_claimed)
        gpio_put(s_pin, 0);
}
