#include <platform/platform.hpp>
#include "picodmg.hpp"
#include "mk_st7789.h"
#include "p8_audio.h"
#include "pico8_menu.h"
#include "dmg_menu.h"
#include "dmg_panel.h"
#include "sdcard.h"
#include "ff.h"

#include <cstdio>
#include <hardware/clocks.h>
#include <hardware/vreg.h>
#include <pico/stdlib.h>
#include <emu/alloc.hpp>
#include <emu/emulator.hpp>
#include <emu/tlsf.hpp>
#include <devices/image.hpp>
#include <devices/screenpalette.hpp>
#include <devices/sfx.hpp>
#include <devices/music.hpp>
#include <video/palette.hpp>
#include <hardwarestate.hpp>

#ifndef GB_PSRAM_CS_PIN
#define GB_PSRAM_CS_PIN 47
#endif

#ifndef GB_LED_GPIO
#define GB_LED_GPIO 39
#endif

extern "C" int32_t SetupPsram(int psramCS);

extern "C" {
extern char __heap_start, __heap_end;
extern char __psram_start, __psram_heap_start;
}

namespace arch::pico::platform {

namespace picodmg {

void init_default_frequency() {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(2);
    set_sys_clock_khz(280000, true);
    sleep_ms(2);

    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
}

void init_stdio() {
    stdio_init_all();
}

void init_basic_gpio() {
    gpio_init(GB_LED_GPIO);
    gpio_set_dir(GB_LED_GPIO, GPIO_OUT);
    gpio_put(GB_LED_GPIO, true);

    dmg_init_ui_buttons();
}

std::size_t init_psram_picodmg() {
    int32_t psram_bytes = SetupPsram(GB_PSRAM_CS_PIN);
    if (psram_bytes <= 0) {
        printf("[PicoDMG] No PSRAM detected on GP%d\n", GB_PSRAM_CS_PIN);
        return 0;
    }
    printf("[PicoDMG] PSRAM initialized: %ld bytes (%ld MB)\n",
           (long)psram_bytes, (long)(psram_bytes / (1024 * 1024)));
    return (std::size_t)psram_bytes;
}

void init_video_picodmg() {
    dmg_panel_config_init();
    dmg_init_boot_mode();
    mk_ili9225_init();
    mk_ili9225_fill(0x0000);
}

} // namespace picodmg

void init_hardware() {
    using namespace picodmg;

    init_stdio();
    printf("\n========================================\n");
    printf("[Pico8] PicoDMG PICO-8 Runner\n");
    printf("========================================\n");

    printf("  Heap#1 size: %d bytes\n", &__heap_end - &__heap_start);
    global_heap = tlsf_create_with_pool(&__heap_start, &__heap_end - &__heap_start);

    init_default_frequency();
    /* On PicoDMG, rendering runs directly on Core 0 to avoid multicore FIFO deadlocks
     * and maintain single-core ownership of the SPI0 bus. */
    // arch::pico::init_cmd_thread();
    init_basic_gpio();

    /* MicroSD init via FatFs_SPI */
    bool sd_ok = false;
    sd_card_t *pSD = sd_get_by_num(0);
    if (pSD) {
        FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
        if (fr != FR_OK) {
            sleep_ms(50);
            fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
        }
        sd_ok = (fr == FR_OK);
    }
    printf("[PicoDMG] MicroSD mount: %s\n", sd_ok ? "OK" : "FAILED");

    /* Video init with boot.txt options */
    init_video_picodmg();

    /* PSRAM dual-heap init */
    const auto psram_size = init_psram_picodmg();
    if (psram_size > 0) {
        const auto psram_reserved = &__psram_heap_start - &__psram_start;
        const std::size_t heap_size = psram_size - psram_reserved;
        printf("  Heap#2 (PSRAM fallback) size: %d bytes (reserved buffers: %d)\n", heap_size, psram_reserved);
        extmem_heap = tlsf_create_with_pool(&__psram_heap_start, heap_size);
    }

    /* I2S Audio init */
    p8_audio_init();

    /* PicoDMG shared_menu init */
    pico8_menu_init();

    /* Initialize emulator core */
    emu::emulator.init();

    /* Connect PICO-8 sound hardware memory tables to audio synthesizer */
    p8_audio_set_memory(
        emu::emulator.memory().raw_view.data() + devices::Sfx::default_map_address,
        emu::emulator.memory().raw_view.data() + devices::Music::default_map_address
    );

    printf("[Pico8] Hardware initialization complete!\n");
}

void present_frame(FrameCopiedCallback *callback) {
    const uint8_t *fb_4bit = emu::emulator.memory().raw_view.data() + devices::Framebuffer::default_map_address;
    
    /* Convert active PICO-8 screen palette to RGB565 */
    uint16_t pal_rgb565[16];
    auto scr_pal = emu::device<devices::ScreenPalette>;
    for (int i = 0; i < 16; i++) {
        uint8_t col_idx = scr_pal.get_color(i);
        uint32_t rgb8 = emu::emulator.palette()[col_idx & 0x1F];
        /* RGB888 to RGB565 */
        uint8_t r = (rgb8 >> 16) & 0xFF;
        uint8_t g = (rgb8 >> 8) & 0xFF;
        uint8_t b = rgb8 & 0xFF;
        pal_rgb565[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    uint8_t fps_val = pico8_menu_get_fps() ? 30 : 0;
    mk_st7789_draw_pico8_frame(fb_4bit, pal_rgb565, fps_val);

    if (callback != nullptr) {
        callback();
    }
}

void local_core_init() {
}

std::span<const std::uint32_t, 32> get_default_palette() {
    return ::video::pico8_palette_rgb8;
}

} // namespace arch::pico::platform
