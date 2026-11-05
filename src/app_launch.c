/*
 * app_launch.c - Hand off control to the application located at APP_BASE_ADDR.
 * Arm Cortex-M33 images only: the app is entered through its vector table.
 */
#include "app_launch.h"

#include "boot_config.h"
#include "pico/stdlib.h"
#include "hardware/resets.h"

int g_app_launch_error = 0;

/* CMSIS core symbols (SCB, NVIC, SysTick, __set_MSP, ...) come from the RP2350
 * CMSIS device header. It is not pulled in transitively here, so include it
 * explicitly (the cmsis_core library supplies the include path). */
#include "RP2350.h"

static const uint32_t *app_vectors(void)
{
    return (const uint32_t *)APP_BASE_ADDR;
}

bool app_launch_present(void)
{
    const uint32_t *vt = app_vectors();
    uint32_t sp    = vt[0];
    uint32_t reset = vt[1];

    /* Erased flash reads as all-ones. */
    if (sp == 0xFFFFFFFFu && reset == 0xFFFFFFFFu)
        return false;

    /* Arm Cortex-M vector table: SP in SRAM, word aligned; reset handler
     * inside the app partition, Thumb bit set. */
    return sp >= SRAM_BASE && sp <= SRAM_END_ADDR && !(sp & 0x3u) &&
           reset >= APP_BASE_ADDR && reset < APP_END_ADDR && (reset & 0x1u);
}

void app_launch_run(void)
{
    g_app_launch_error = 0;
    if (!app_launch_present()) {
        g_app_launch_error = -1;
        return;
    }

    stdio_flush();

    const uint32_t *vt = app_vectors();
    uint32_t app_sp    = vt[0];
    uint32_t app_reset = vt[1];

    __disable_irq();

    /* Put PIO0 and the DMA block back in their power-on state. The power-on
     * chime runs I2S on PIO0 fed by DMA; left running, its state machine and
     * channel would carry over into the app, which expects to claim and
     * program them from scratch exactly as after a cold boot. */
    reset_block(RESETS_RESET_PIO0_BITS | RESETS_RESET_DMA_BITS);
    unreset_block_wait(RESETS_RESET_PIO0_BITS | RESETS_RESET_DMA_BITS);

    /* Stop and clear SysTick. */
    SysTick->CTRL = 0;
    SysTick->VAL  = 0;

    /* Disable and clear all NVIC interrupts the bootloader may have enabled. */
    for (unsigned i = 0; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    /* Point the CPU at the application's vector table. */
    SCB->VTOR = APP_BASE_ADDR;

    /* ARMv8-M: clear the main stack-limit so the app can set its own, switch to
     * the MSP in privileged thread mode, then load the app's stack pointer. */
    __set_MSPLIM(0);
    __set_CONTROL(0);
    __ISB();
    __set_MSP(app_sp);
    __DSB();
    __ISB();

    /* Re-enable interrupts; the application's runtime init will configure
     * priorities. (SDK >= 2.1.0 also re-enables IRQs during runtime init.) */
    __enable_irq();

    /* Branch to the application's reset handler. Never returns. */
    ((void (*)(void))app_reset)();

    __builtin_unreachable();
}
