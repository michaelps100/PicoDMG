#include "dmg_battery.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"

/* R1 = R2, so the pin sees half the cell. */
#define BATT_DIVIDER_RATIO 2u
#define BATT_ADC_VREF_MV   3300u
#define BATT_ADC_MAX       4095u
#define BATT_SAMPLES       16u

static bool    s_configured = false;
static uint8_t s_pin = 0;

/* A pin the board already drives cannot double as the sense input. The macros
 * are compile definitions of the emulator target that compiles this file, so
 * each build checks against its own wiring. */
static bool pin_in_use(uint8_t pin)
{
    if (pin >= 2u && pin <= 9u) return true;   /* console buttons */
#if defined(PSRAM_CS_PIN)
    if (PSRAM_CS_PIN > 0 && pin == (uint8_t)PSRAM_CS_PIN) return true;
#endif
#if defined(PICO_AUDIO_I2S_DATA_PIN)
    if (pin == (uint8_t)PICO_AUDIO_I2S_DATA_PIN) return true;
#endif
#if defined(PICO_AUDIO_I2S_CLOCK_PIN_BASE)
    if (pin == (uint8_t)PICO_AUDIO_I2S_CLOCK_PIN_BASE ||
        pin == (uint8_t)(PICO_AUDIO_I2S_CLOCK_PIN_BASE + 1)) return true;
#endif
    return false;
}

void dmg_battery_init_pin(uint8_t pin)
{
    if (s_configured || pin == 0u) return;

    /* The last channel is the internal temperature sensor, not a pin. */
    const unsigned first = ADC_BASE_PIN;
    const unsigned last  = ADC_BASE_PIN + NUM_ADC_CHANNELS - 2u;
    if (pin < first || pin > last) {
        printf("[battery] BATT_PIN=%u is not an ADC pin on this chip (%u..%u) -- disabled\n",
               (unsigned)pin, first, last);
        return;
    }
    if (pin_in_use(pin)) {
        printf("[battery] BATT_PIN=%u is already used by this board -- disabled\n",
               (unsigned)pin);
        return;
    }

    adc_init();
    /* Disables the digital input buffer and the pulls: the divider is high
     * impedance, and on the RP2350 a pad with its input buffer on leaks
     * (erratum E9). */
    adc_gpio_init(pin);
    s_pin = pin;
    s_configured = true;
    printf("[battery] sensing on GPIO%u (ADC%u)\n", (unsigned)pin, (unsigned)(pin - first));
}

bool dmg_battery_configured(void)
{
    return s_configured;
}

uint16_t dmg_battery_read_mv(void)
{
    if (!s_configured) return 0u;

    adc_select_input((uint)(s_pin - ADC_BASE_PIN));
    /* First conversion after a mux change can carry the previous channel. */
    (void)adc_read();

    uint32_t acc = 0;
    for (unsigned i = 0; i < BATT_SAMPLES; i++) {
        acc += adc_read();
    }
    const uint32_t raw = acc / BATT_SAMPLES;
    return (uint16_t)((raw * BATT_ADC_VREF_MV * BATT_DIVIDER_RATIO) / BATT_ADC_MAX);
}

uint8_t dmg_battery_percent(uint16_t mv)
{
    /* Resting-voltage curve, measured on this console. The old table put 100%
     * at 4.20 V, which is the TP4056 charge voltage, not where the cell sits:
     * a cell charged overnight reads ~4.08 V through the divider once the
     * charger is unplugged (surface charge gone, plus the 1% divider and ADC
     * gain error), so the marker never went past 88%. 100% is now 4.06 V. */
    static const uint16_t tbl_mv[]  = { 3300, 3450, 3550, 3650, 3720, 3770, 3810, 3860, 3910, 3960, 4010, 4060 };
    static const uint8_t  tbl_pct[] = {    0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100 };
    const unsigned n = (unsigned)(sizeof(tbl_mv) / sizeof(tbl_mv[0]));

    if (mv <= tbl_mv[0]) return 0u;
    if (mv >= tbl_mv[n - 1u]) return 100u;
    for (unsigned i = 1; i < n; i++) {
        if (mv <= tbl_mv[i]) {
            const uint32_t span_mv  = (uint32_t)(tbl_mv[i] - tbl_mv[i - 1u]);
            const uint32_t span_pct = (uint32_t)(tbl_pct[i] - tbl_pct[i - 1u]);
            const uint32_t off      = (uint32_t)(mv - tbl_mv[i - 1u]);
            return (uint8_t)(tbl_pct[i - 1u] + (off * span_pct) / span_mv);
        }
    }
    return 100u;
}

/* =========================================================================
 * Charger sense: the TP4056 module's 5 V input (IN+) through a 1/2 divider.
 * High = USB connected. Plain digital input, no pulls: the divider defines
 * both levels.
 * ========================================================================= */
static bool    s_chg_configured = false;
static uint8_t s_chg_pin = 0;

void dmg_battery_init_charge_pin(uint8_t pin)
{
    if (s_chg_configured || pin == 0u) return;
    if (pin >= NUM_BANK0_GPIOS || pin_in_use(pin) || (s_configured && pin == s_pin)) {
        printf("[battery] CHARGE_PIN=%u is not usable on this board -- disabled\n", (unsigned)pin);
        return;
    }
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
    s_chg_pin = pin;
    s_chg_configured = true;
    printf("[battery] charger sense on GPIO%u\n", (unsigned)pin);
}

bool dmg_battery_charge_configured(void)
{
    return s_chg_configured;
}

bool dmg_battery_usb_present(void)
{
    if (!s_chg_configured) return false;
    /* Three agreeing reads, so a plug being wiggled does not flip the icon. */
    for (int i = 0; i < 3; i++) {
        if (!gpio_get(s_chg_pin)) return false;
        busy_wait_us(50);
    }
    return true;
}
