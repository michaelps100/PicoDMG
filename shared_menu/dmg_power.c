#include "dmg_power.h"
#include "dmg_menu.h"
#include "dmg_panel.h"
#include "dmg_resume.h"
#include "dmg_battery.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/xosc.h"
#include "hardware/rosc.h"
#include "hardware/pll.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "ff.h"

/* Display & Backlight hardware pins */
#define LCD_SPI     spi0
#define LCD_PIN_CS  17
#define LCD_PIN_DC  20
#define LCD_PIN_BL  22

/* State */
static bool                 s_power_inited = false;
static bool                 s_power_enabled = false;
static uint8_t              s_power_pin = 10;
static dmg_power_behavior_t s_power_behavior = DMG_POWER_BEHAVIOR_RESUME;
static bool                 s_power_active_low = true; /* true: 0=ON (GND), 1=OFF (Open) */
static const dmg_emulator_desc_t *s_last_desc = NULL;

static inline void lcd_send_cmd8(uint8_t cmd)
{
    while (spi_is_busy(LCD_SPI)) {
        tight_loop_contents();
    }
    gpio_put(LCD_PIN_CS, 0);
    gpio_put(LCD_PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &cmd, 1);
    while (spi_is_busy(LCD_SPI)) {
        tight_loop_contents();
    }
    gpio_put(LCD_PIN_CS, 1);
}

static bool parse_u8(const char *s, uint8_t *out)
{
    if (!s || s[0] == '\0') return false;
    unsigned v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 255u) return false;
    }
    *out = (uint8_t)v;
    return true;
}

void dmg_power_init(void)
{
    if (s_power_inited) return;

    static const char *const paths[] = {
        "0:/boot.txt", "/boot.txt", "boot.txt", "0:boot.txt",
    };
    const unsigned npaths = (unsigned)(sizeof(paths) / sizeof(paths[0]));

    FIL fil;
    bool opened = false;
    FRESULT fr = FR_NOT_READY;
    for (unsigned i = 0; i < npaths; i++) {
        fr = f_open(&fil, paths[i], FA_READ);
        if (fr == FR_OK) { opened = true; break; }
    }

    if (!opened) {
        if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
            s_power_inited = true; /* file absent: defaults stand (disabled) */
        }
        return; /* wait until SD is mounted */
    }

    s_power_inited = true;

    bool seen_explicit_switch = false;
    char line[192];
    while (f_gets(line, sizeof(line), &fil)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;

        /* trim key tail */
        size_t kl = strlen(p);
        while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) p[--kl] = '\0';
        /* trim value both ends */
        while (*val == ' ' || *val == '\t') val++;
        size_t vl = strlen(val);
        while (vl > 0 && (val[vl - 1] == ' ' || val[vl - 1] == '\t' ||
                          val[vl - 1] == '\r' || val[vl - 1] == '\n')) {
            val[--vl] = '\0';
        }
        if (*val == '\0') continue;

        if (strcasecmp(p, "POWER_SWITCH") == 0) {
            s_power_enabled = (val[0] == '1');
            seen_explicit_switch = true;
        } else if (strcasecmp(p, "POWER_PIN") == 0) {
            uint8_t pin;
            if (parse_u8(val, &pin) && pin <= 47) {
                s_power_pin = pin;
                if (!seen_explicit_switch) {
                    s_power_enabled = true;
                }
            }
        } else if (strcasecmp(p, "POWER_BEHAVIOR") == 0) {
            if (strcasecmp(val, "REBOOT") == 0) {
                s_power_behavior = DMG_POWER_BEHAVIOR_REBOOT;
            } else {
                s_power_behavior = DMG_POWER_BEHAVIOR_RESUME;
            }
        } else if (strcasecmp(p, "POWER_ACTIVE_LOW") == 0) {
            s_power_active_low = (val[0] != '0');
        }
    }
    f_close(&fil);

    if (s_power_enabled) {
        gpio_init(s_power_pin);
        gpio_set_dir(s_power_pin, GPIO_IN);
        if (s_power_active_low) {
            gpio_pull_up(s_power_pin);
        } else {
            gpio_pull_down(s_power_pin);
        }
        printf("[dmg_power] Power switch active on GP%u (active_%s, behavior=%s)\n",
               s_power_pin, s_power_active_low ? "low" : "high",
               (s_power_behavior == DMG_POWER_BEHAVIOR_REBOOT) ? "REBOOT" : "RESUME");
    }
}

bool dmg_power_is_enabled(void)
{
    if (!s_power_inited) dmg_power_init();
    return s_power_enabled;
}

uint8_t dmg_power_get_pin(void)
{
    return s_power_pin;
}

dmg_power_behavior_t dmg_power_get_behavior(void)
{
    return s_power_behavior;
}

/* Standby-by-save-state state (see dmg_power.h) */
static const dmg_emulator_desc_t *s_resume_desc = NULL;
static bool  s_in_pause_menu = false;
static bool  s_game_set = false;
/* Pointers, not copies: the emulator passes its own long-lived ROM-name buffer
 * (PicoWS has no SRAM to spare for a second copy). */
static const char *s_game_rom  = "";
static const char *s_game_name = "";

/* Power switch arming & delay state */
static bool     s_power_armed = false;
static uint64_t s_power_allow_standby_us = 0;

void dmg_power_arm_menu_loaded(uint32_t delay_ms)
{
    s_power_armed = true;
    s_power_allow_standby_us = time_us_64() + ((uint64_t)delay_ms * 1000ULL);
    printf("[dmg_power] Menu fully loaded: power switch armed with %u ms delay\n", (unsigned)delay_ms);
}

bool dmg_power_is_ready(void)
{
    if (!s_power_armed) {
        /* Failsafe: if an application doesn't use the standard ROM selector and
         * has been running for more than 5 seconds, auto-arm with 1s delay */
        if (time_us_64() > 5000000ULL) {
            s_power_armed = true;
            s_power_allow_standby_us = time_us_64() + 1000000ULL;
            return false;
        }
        return false;
    }
    return (time_us_64() >= s_power_allow_standby_us);
}

bool dmg_power_poll(const dmg_emulator_desc_t *desc)
{
    if (!s_power_inited) {
        dmg_power_init();
    }
    if (!s_power_enabled) {
        return false;
    }
    if (desc != NULL) {
        s_last_desc = desc;
    }

    /* Guard: hold off standby until emulator menu has fully loaded + delay */
    if (!dmg_power_is_ready()) {
        return false;
    }

    /* Read switch position: true = user placed switch into OFF position */
    bool pin_raw = gpio_get(s_power_pin);
    bool is_off = s_power_active_low ? pin_raw : !pin_raw;

    if (!is_off) {
        return false;
    }

    /* Debounce: verify the switch stays OFF for 30 ms */
    for (int i = 0; i < 3; i++) {
        sleep_ms(10);
        watchdog_update();
        pin_raw = gpio_get(s_power_pin);
        bool check_off = s_power_active_low ? pin_raw : !pin_raw;
        if (!check_off) {
            return false; /* Glitch or mechanical bounce */
        }
    }

    /* Confirmed OFF. An emulator that opted in to standby-by-save-state goes
     * through dmg_power_off_with_resume(), which never returns. From a game
     * frame the combo layer checks dmg_power_switch_off_confirmed() itself and
     * turns the OFF into request_power_off, so the save happens at a frame
     * boundary; this path only runs from menus (pause menu included, where the
     * game is parked at a frame boundary too). */
    if (s_resume_desc != NULL) {
        dmg_power_off_with_resume(desc, DMG_POWER_REASON_SWITCH);
    }

    /* Confirmed OFF: enter low-power standby mode */
    dmg_power_enter_standby(desc);
    return true;
}

/* Switch position read with the configured polarity. */
static bool switch_is_off(void)
{
    bool pin_raw = gpio_get(s_power_pin);
    return s_power_active_low ? pin_raw : !pin_raw;
}

/* Steps shared by both standby flavours. stop_blitter is idempotent, so calling
 * it from a menu (where the blitter is already parked) is harmless. */
static void standby_sleep(const dmg_emulator_desc_t *desc)
{
    /* 1. Stop Core 1 blitter if available */
    if (desc && desc->display.stop_blitter) {
        desc->display.stop_blitter();
    }

    /* 2. Mute audio to avoid pops/hiss */
    if (desc && desc->apply_volume) {
        desc->apply_volume(0);
    }
    sleep_ms(10);

    /* 3. Turn off display backlight immediately */
    gpio_set_function(LCD_PIN_BL, GPIO_FUNC_SIO);
    gpio_set_dir(LCD_PIN_BL, GPIO_OUT);
    gpio_put(LCD_PIN_BL, 0);

    /* 3b. Black GRAM. The panel keeps its GRAM through SLPIN, DORMANT and even
     * a reset, so without this the last game frame is what shows when the
     * panel comes back up on wake -- before the intro, or under it. Done with
     * the backlight already off, so the wipe itself is never seen. */
    if (desc && desc->display.fill_screen) {
        desc->display.fill_screen(0x0000);
    }

    /* 4. Put display panel to sleep: DISPOFF (0x28) and SLPIN (0x10) */
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_init(LCD_PIN_CS);
    gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
    gpio_init(LCD_PIN_DC);
    gpio_set_dir(LCD_PIN_DC, GPIO_OUT);

    lcd_send_cmd8(0x28); /* DISPOFF */
    sleep_ms(5);
    lcd_send_cmd8(0x10); /* SLPIN */
    sleep_ms(10);
}

static void standby_wake(const dmg_emulator_desc_t *desc, bool restart_blitter)
{
    /* 6. Handle wake behavior */
    if (s_power_behavior == DMG_POWER_BEHAVIOR_REBOOT) {
        printf("[dmg_power] Rebooting cleanly into bootloader...\n");
        watchdog_reboot(0, 0, 0);
        while (true) {
            tight_loop_contents();
        }
    }

    printf("[dmg_power] Resuming from standby...\n");

    /* 7. Wake display: SLPOUT (0x11) */
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    lcd_send_cmd8(0x11); /* SLPOUT */
    sleep_ms(120);
    watchdog_update();

    /* 8. Turn display ON: DISPON (0x29) */
    lcd_send_cmd8(0x29); /* DISPON */
    sleep_ms(10);
    watchdog_update();

    /* 9. Restart Core 1 blitter if available */
    if (restart_blitter && desc && desc->display.start_blitter) {
        desc->display.start_blitter();
    }

    /* 10. Restore backlight */
    uint8_t br = (g_dmg_config.brightness_level >= 1 && g_dmg_config.brightness_level <= 5) ?
                 (uint8_t)g_dmg_config.brightness_level : 5;
    if (desc && desc->apply_brightness) {
        desc->apply_brightness(br);
    } else {
        gpio_put(LCD_PIN_BL, 1);
    }

    /* 11. Restore audio volume */
    if (desc && desc->apply_volume) {
        desc->apply_volume(dmg_get_global_volume());
    }

    printf("[dmg_power] Resumed successfully!\n");
}

void dmg_power_enter_standby(const dmg_emulator_desc_t *desc)
{
    if (desc != NULL) {
        s_last_desc = desc;
    } else {
        desc = s_last_desc;
    }

    printf("[dmg_power] Power switch OFF: entering standby...\n");
    standby_sleep(desc);

    /* 5. Standby loop: idle in low power while switch is in OFF position */
    while (true) {
        sleep_ms(20);
        watchdog_update();

        if (!switch_is_off()) {
            /* Debounce switch return to ON (30 ms) */
            bool still_on = true;
            for (int i = 0; i < 3; i++) {
                sleep_ms(10);
                watchdog_update();
                if (switch_is_off()) {
                    still_on = false;
                    break;
                }
            }
            if (still_on) {
                break; /* Switch confirmed ON */
            }
        }
    }

    standby_wake(desc, true);
}

void dmg_power_enter_battery_standby(const dmg_emulator_desc_t *desc, bool (*charger_present)(void))
{
    if (desc != NULL) {
        s_last_desc = desc;
    } else {
        desc = s_last_desc;
    }

    printf("[dmg_power] Battery low: entering standby...\n");
    standby_sleep(desc);

    /* The switch is normally still ON here. Wake on a charger being plugged
     * in, or on the switch being cycled OFF -> ON (the player's "turn it off
     * and on again"). Without either configured, only a power cycle exits. */
    bool seen_off = false;
    while (true) {
        sleep_ms(50);
        watchdog_update();

        if (charger_present != NULL && charger_present()) {
            printf("[dmg_power] Charger connected: waking\n");
            break;
        }
        if (s_power_enabled) {
            const bool off = switch_is_off();
            if (off) {
                seen_off = true;
            } else if (seen_off) {
                sleep_ms(30);
                watchdog_update();
                if (!switch_is_off()) {
                    printf("[dmg_power] Switch cycled: waking\n");
                    break;
                }
            }
        }
    }

    /* Always called from a menu: the blitter stays parked and the caller
     * repaints the screen. */
    standby_wake(desc, false);
}

/* =========================================================================
 * Standby by save state
 * ========================================================================= */

void dmg_power_enable_resume(const dmg_emulator_desc_t *desc) {
    s_resume_desc = desc;
    if (desc != NULL) s_last_desc = desc;
}

bool dmg_power_resume_enabled(void) {
    return s_resume_desc != NULL;
}

void dmg_power_set_game(const char *rom_file, const char *game_name) {
    if (rom_file == NULL || rom_file[0] == '\0') {
        s_game_set = false;
        s_game_rom = s_game_name = "";
        return;
    }
    s_game_rom  = rom_file;
    s_game_name = game_name ? game_name : rom_file;
    s_game_set  = true;
}

void dmg_power_note_pause_menu(bool in_menu) {
    s_in_pause_menu = in_menu;
}

bool dmg_power_switch_off_confirmed(void) {
    if (!s_power_inited) dmg_power_init();
    if (!s_power_enabled) return false;
    if (!dmg_power_is_ready()) return false;
    if (!switch_is_off()) return false;
    for (int i = 0; i < 3; i++) {
        sleep_ms(10);
        watchdog_update();
        if (!switch_is_off()) return false;
    }
    return true;
}

/* DORMANT until one of up to two GPIO events. Everything still running on its
 * own clock has to be quiet first: core 1 is reset outright (we reboot on wake,
 * so nothing on it is worth keeping), then the same clock sequence the
 * bootloader's own standby has always run (pico-bootLoader checkPowerSwitch). */
static void dormant_until(uint pin_a, uint32_t event_a, int pin_b, uint32_t event_b) {
    multicore_reset_core1();
    watchdog_disable();

    gpio_set_input_enabled(pin_a, true);
    gpio_acknowledge_irq(pin_a, event_a);
    gpio_set_dormant_irq_enabled(pin_a, event_a, true);
    if (pin_b >= 0) {
        gpio_set_input_enabled((uint)pin_b, true);
        gpio_acknowledge_irq((uint)pin_b, event_b);
        gpio_set_dormant_irq_enabled((uint)pin_b, event_b, true);
    }

    clock_configure_undivided(clk_ref, CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC, 0, XOSC_HZ);
    clock_configure_undivided(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF, 0, XOSC_HZ);
    clock_stop(clk_adc);
    clock_stop(clk_usb);
#if PICO_RP2350
    clock_stop(clk_hstx);
#endif
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, XOSC_HZ, XOSC_HZ);
    pll_deinit(pll_sys);
    pll_deinit(pll_usb);
    /* The emulators raise the core to 1.30 V for 360 MHz. At 12 MHz the
     * default is plenty, and leakage is what is left to save in DORMANT. The
     * next boot sets its own voltage before raising the clock again. */
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    rosc_disable();

    xosc_dormant();

    gpio_set_dormant_irq_enabled(pin_a, event_a, false);
    gpio_acknowledge_irq(pin_a, event_a);
    if (pin_b >= 0) {
        gpio_set_dormant_irq_enabled((uint)pin_b, event_b, false);
        gpio_acknowledge_irq((uint)pin_b, event_b);
    }
}

static bool charger_in(void) {
    return dmg_battery_charge_configured() && dmg_battery_usb_present();
}

void dmg_power_sleep_and_reboot(dmg_power_reason_t reason) {
    if (!s_power_inited) dmg_power_init();
    const uint32_t lvl_on  = s_power_active_low ? GPIO_IRQ_LEVEL_LOW  : GPIO_IRQ_LEVEL_HIGH;
    const uint32_t lvl_off = s_power_active_low ? GPIO_IRQ_LEVEL_HIGH : GPIO_IRQ_LEVEL_LOW;

    printf("[dmg_power] DORMANT (%s)\n", reason == DMG_POWER_REASON_BATTERY ? "battery" : "switch");
    stdio_flush();

    if (reason == DMG_POWER_REASON_BATTERY) {
        /* Wake on a charger being plugged in, or on the switch being turned
         * OFF -- the first half of "turn it off and on again". */
        dmg_battery_init_charge_pin(dmg_panel_get_charge_pin());
        const int chg = dmg_battery_charge_configured() ? (int)dmg_panel_get_charge_pin() : -1;
        if (s_power_enabled) {
            dormant_until(s_power_pin, lvl_off, chg, GPIO_IRQ_LEVEL_HIGH);
        } else if (chg >= 0) {
            dormant_until((uint)chg, GPIO_IRQ_LEVEL_HIGH, -1, 0);
        } else {
            /* Nothing configured can wake us: only a power cycle exits. */
            dormant_until(s_power_pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, -1, 0);
        }
        if (!charger_in() && s_power_enabled && switch_is_off()) {
            dormant_until(s_power_pin, lvl_on, -1, 0);
        }
    } else if (s_power_enabled) {
        dormant_until(s_power_pin, lvl_on, -1, 0);
    }

    /* Debounce the switch on the way up: a bounce back to OFF sleeps again. */
    for (int tries = 0; s_power_enabled && reason == DMG_POWER_REASON_SWITCH && tries < 8; tries++) {
        busy_wait_ms(30);
        if (!switch_is_off()) break;
        dormant_until(s_power_pin, lvl_on, -1, 0);
    }

    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

void dmg_power_off_with_resume(const dmg_emulator_desc_t *desc, dmg_power_reason_t reason) {
    if (desc == NULL) desc = s_resume_desc ? s_resume_desc : s_last_desc;

    printf("[dmg_power] %s: save and sleep (%s)\n",
           reason == DMG_POWER_REASON_BATTERY ? "Battery low" : "Power switch OFF",
           s_game_set ? (s_in_pause_menu ? "pause menu" : "in game") : "menu");

    /* Screen dark first: it is the feedback that the switch was seen, and the
     * save below can take a second or two on the bigger systems. */
    standby_sleep(desc);

    dmg_resume_kind_t kind = DMG_RESUME_MENU;
    if (s_game_set && desc != NULL && desc->save_state != NULL) {
        watchdog_update();
        const bool saved = desc->save_state(s_game_name, DMG_RESUME_SLOT);
        watchdog_update();
        /* The cartridge battery save goes too: the state restores RAM, but a
         * power cycle before the resume would otherwise lose the .sav. */
        if (desc->flush_saves) desc->flush_saves();
        else if (desc->exit_game) desc->exit_game();
        watchdog_update();

        if (saved) {
            dmg_resume_record_t rec;
            memset(&rec, 0, sizeof(rec));
            snprintf(rec.app, sizeof(rec.app), "%s", desc->app_title ? desc->app_title : "");
            snprintf(rec.rom_dir, sizeof(rec.rom_dir), "%s", dmg_get_current_rom_dir());
            snprintf(rec.rom_file, sizeof(rec.rom_file), "%s", s_game_rom);
            snprintf(rec.game_name, sizeof(rec.game_name), "%s", s_game_name);
            if (dmg_resume_write_record(&rec)) kind = DMG_RESUME_GAME;
        } else {
            printf("[dmg_power] save state failed: waking to the menu\n");
        }
    }

    dmg_resume_set_mark(kind, (uint8_t)reason);
    dmg_power_sleep_and_reboot(reason);
}
