#include "ili9341.h"
#include "dmg_menu.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/dma.h"

#include "font_8x8.h"

/* InfoNES palette table reference for backdrop color */
extern uint16_t PalTable[];

/* Power-on sequences, picked by panel in ili9341_init(). Defined after it so
 * the GPIO/SPI bring-up stays in one place. */
static void st7789_init_sequence(void);
static void ili9341_init_sequence(void);

/* ILI9341 Commands */
#define CMD_SWRESET  0x01
#define CMD_SLPOUT   0x11
#define CMD_NORON    0x13
#define CMD_INVOFF   0x20
#define CMD_INVON    0x21
#define CMD_GAMSET   0x26
#define CMD_DISPON   0x29
#define CMD_CASET    0x2A
#define CMD_PASET    0x2B
#define CMD_RAMWR    0x2C
#define CMD_MADCTL   0x36
#define CMD_COLMOD   0x3A
#define CMD_FRMCTR1  0xB1
#define CMD_INVCTR   0xB4
#define CMD_DFUNCTR  0xB6
#define CMD_PWCTR1   0xC0
#define CMD_PWCTR2   0xC1
#define CMD_PWCTR3   0xC2
#define CMD_PWCTR4   0xC3
#define CMD_PWCTR5   0xC4
#define CMD_VMCTR1   0xC5
#define CMD_VMCTR2   0xC7
#define CMD_GMCTRP1  0xE0
#define CMD_GMCTRN1  0xE1

#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_ML  0x10
#define MADCTL_BGR 0x08
#define MADCTL_MH  0x04

#define SPI_INIT_BAUD  30000000u
#define SPI_RUN_BAUD   80000000u

/* Dedicated DMA channel and double-buffered ping-pong line buffers for SPI */
static int s_spi_dma_chan = -1;
static dma_channel_config s_spi_dma_cfg;
static uint8_t __aligned(4) s_dma_line_buf[2][ILI9341_WIDTH * 2u];

static void ili9341_dma_init(void) {
    if (s_spi_dma_chan < 0) {
        s_spi_dma_chan = dma_claim_unused_channel(false);
        if (s_spi_dma_chan >= 0) {
            s_spi_dma_cfg = dma_channel_get_default_config(s_spi_dma_chan);
            channel_config_set_transfer_data_size(&s_spi_dma_cfg, DMA_SIZE_8);
            channel_config_set_dreq(&s_spi_dma_cfg, spi_get_dreq(ILI9341_SPI, true));
            channel_config_set_read_increment(&s_spi_dma_cfg, true);
            channel_config_set_write_increment(&s_spi_dma_cfg, false);
            dma_channel_configure(
                s_spi_dma_chan,
                &s_spi_dma_cfg,
                &spi_get_hw(ILI9341_SPI)->dr,
                NULL,
                0,
                false
            );
        }
    }
}

/* ------------------------------------------------------------------ */
/* Shared display memory in SRAM (saves 120 KB RAM)                   */
/* Menu mode: full 320x240 RGB555 framebuffer (153.6 KB)              */
/* Gameplay:  2 x 240 x 256 Tear-Free Double Buffer (245.76 KB)        */
/* ------------------------------------------------------------------ */
#define NES_NATIVE_WIDTH   256u
#define NES_NATIVE_HEIGHT  240u

static union {
    uint16_t fb[ILI9341_WIDTH * ILI9341_HEIGHT];
#if SFR_SLIM
    uint16_t nes_buf[1][1];
#else
    uint16_t nes_buf[2][NES_NATIVE_HEIGHT * NES_NATIVE_WIDTH];
#endif
} __aligned(4) s_display_mem;

#define s_fb        (s_display_mem.fb)
#define s_nes_buf   (s_display_mem.nes_buf)
static volatile uint8_t s_write_buf_idx = 0u;
static volatile uint8_t s_render_buf_idx = 0u;
static volatile uint8_t s_field_parity = 0u;
static volatile uint8_t s_render_field_parity = 0u;
static volatile bool s_field_ready = false;
static volatile bool s_blitter_core1_launched = false;
static volatile bool s_blitter_active = false;
static volatile bool s_scanlines_enabled = false;
static ili9341_scale_mode_t s_scale_mode = SCALE_FIT;
static bool s_height_fill = true; /* true: stretch active 224 lines (8..231) to 240 lines */
static uint8_t s_crop_sides = 1;  /* 0 = Off, 1 = 8px Mask (L+R), 2 = 8px Fill */

static bool s_nes_interleave = true; /* true: 120 lines/frame (Interleave 60fps), false: 240 lines (Progressive) */
static uint16_t s_band_xmap[ILI9341_WIDTH];
static bool s_band_xmap_ready = false;
static bool s_shimmerless_enabled = true;
static dmg_scale_weight_t s_band_xweights[ILI9341_WIDTH];
static uint8_t s_ymap[ILI9341_HEIGHT];
static bool s_ymap_ready = false;
static uint16_t s_current_backdrop555 = 0u;

static inline void cs_low(void)  { gpio_put(ILI9341_PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(ILI9341_PIN_CS, 1); }
static inline void dc_cmd(void)  { gpio_put(ILI9341_PIN_DC, 0); }
static inline void dc_data(void) { gpio_put(ILI9341_PIN_DC, 1); }

static void __not_in_flash_func(write_cmd)(uint8_t cmd) {
    dc_cmd();
    cs_low();
    spi_write_blocking(ILI9341_SPI, &cmd, 1);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    cs_high();
}

static void __not_in_flash_func(write_data)(uint8_t data) {
    dc_data();
    cs_low();
    spi_write_blocking(ILI9341_SPI, &data, 1);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    cs_high();
}

static void __not_in_flash_func(write_data_buf)(const uint8_t *data, size_t len) {
    dc_data();
    cs_low();
    spi_write_blocking(ILI9341_SPI, data, len);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    cs_high();
}

static void __not_in_flash_func(set_addr_window)(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t buf[4];
    write_cmd(CMD_CASET);
    buf[0] = (uint8_t)(x0 >> 8);
    buf[1] = (uint8_t)(x0 & 0xFF);
    buf[2] = (uint8_t)(x1 >> 8);
    buf[3] = (uint8_t)(x1 & 0xFF);
    write_data_buf(buf, 4);

    write_cmd(CMD_PASET);
    buf[0] = (uint8_t)(y0 >> 8);
    buf[1] = (uint8_t)(y0 & 0xFF);
    buf[2] = (uint8_t)(y1 >> 8);
    buf[3] = (uint8_t)(y1 & 0xFF);
    write_data_buf(buf, 4);
}

void ili9341_set_backlight(bool on) {
    gpio_put(ILI9341_PIN_BL, on ? 1 : 0);
}

/* MADCTL for the panel and rotation.
 *
 * The panel/mirror bits live in shared_menu (dmg_panel.h) so all nine emulators
 * agree. This used to hardcode 0x28 for every panel, which is right for the
 * PicoDMG's ILI9341 and wrong for the Core2350B's ST7789 -- it needs MX set
 * (0x68), and the symptom is a mirrored image that looks like a dozen other
 * faults. Anything calling set_rotation before the panel is up is fine: the
 * value is resolved again at power-on.
 */
static void apply_madctl(void) {
    write_cmd(CMD_MADCTL);
    write_data(dmg_panel_get_madctl());
}

/* Adapter for dmg_panel_st7789_timing(), which takes one combined
 * command+parameter primitive, while this driver keeps write_cmd() and
 * write_data_buf() separate. */
static void panel_timing_write(uint8_t cmd, const uint8_t *data, size_t len) {
    write_cmd(cmd);
    if (data != NULL && len > 0) write_data_buf(data, len);
}

void ili9341_set_rotation(uint8_t rotation) {
    /* The rotation index the project uses (1 = landscape, 3 = landscape
     * inverted) is kept for the callers that only care about the two landscape
     * orientations; the MADCTL itself now comes from shared_menu. */
    (void)rotation;
    apply_madctl();
}

/* Fast bitwise RGB555 to RGB565 conversion (preserves bit 15 backdrop flag masking) */
static inline uint16_t rgb555_to_rgb565_fast(uint16_t p) {
    uint16_t c = p & 0x7FFFu;
    return (uint16_t)(((c & 0x7FE0u) << 1) | ((c >> 4) & 0x0020u) | (c & 0x001Fu));
}

static inline uint16_t rgb555_to_rgb565(uint16_t p) {
    return rgb555_to_rgb565_fast(p);
}

/* Returns the active display window coordinates (excluding unused side borders) */
static inline void get_active_window(uint16_t *out_x0, uint16_t *out_x1, uint16_t *out_w) {
    uint16_t x0, x1;
    if (dmg_is_gb_mode()) {
        if (s_scale_mode == SCALE_INTEGER) {
            if (s_crop_sides == 2) {
                x0 = 58; x1 = 297;
            } else {
                x0 = 50; x1 = 305;
            }
        } else {
            x0 = 45; x1 = 310;
        }
    } else {
        if (s_crop_sides == 2) {
            if (s_scale_mode == SCALE_INTEGER) {
                x0 = 40; x1 = 279;
            } else if (s_scale_mode == SCALE_FIT) {
                x0 = 13; x1 = 305;
            } else {
                x0 = 0; x1 = ILI9341_WIDTH - 1;
            }
        } else {
            if (s_scale_mode == SCALE_INTEGER) {
                x0 = 32; x1 = 287;
            } else if (s_scale_mode == SCALE_FIT) {
                x0 = 13; x1 = 305;
            } else {
                x0 = 0; x1 = ILI9341_WIDTH - 1;
            }
        }
    }
    *out_x0 = x0;
    *out_x1 = x1;
    *out_w = (uint16_t)(x1 - x0 + 1u);
}

static void nes_prepare_band_xmap(void) {
    if (s_band_xmap_ready) return;
    for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
        s_band_xweights[x].src_idx = 0xFFFFu;
        s_band_xweights[x].weight1 = 0u;
    }
    if (dmg_is_gb_mode()) {
        if (s_crop_sides == 2) {
            /* CROP_SIDES_8PX_FILL: Crop 8px sides and fill active width (240px from 8..247) */
            if (s_scale_mode == SCALE_INTEGER) {
                /* Centered 240px on 266px window (x=45..310): 13px borders inside window */
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 58 || x >= 298) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        s_band_xmap[x] = (uint16_t)(8u + (x - 58u));
                        s_band_xweights[x].src_idx = s_band_xmap[x];
                    }
                }
            } else {
                /* SCALE_FIT & SCALE_STRETCH: Fill 266px window */
                dmg_scale_weight_t w266[266];
                dmg_init_shimmerless_weights(240, 266, w266);
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 45 || x >= 311) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        uint16_t off = (uint16_t)(x - 45u);
                        s_band_xmap[x] = (uint16_t)(8u + (((uint32_t)off * 240u) / 266u));
                        s_band_xweights[x].src_idx = (uint16_t)(8u + w266[off].src_idx);
                        s_band_xweights[x].weight1 = w266[off].weight1;
                    }
                }
            }
        } else {
            /* Normal 256 pixel native mapping (0..255) */
            if (s_scale_mode == SCALE_INTEGER) {
                /* Centered 256px on 266px window (x=45..310): 5px borders inside window */
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 50 || x >= 306) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        s_band_xmap[x] = (uint16_t)(x - 50u);
                        s_band_xweights[x].src_idx = s_band_xmap[x];
                    }
                }
            } else {
                /* SCALE_FIT & SCALE_STRETCH: Scale 256 across 266px window */
                dmg_scale_weight_t w266[266];
                dmg_init_shimmerless_weights(256, 266, w266);
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 45 || x >= 311) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        uint16_t off = (uint16_t)(x - 45u);
                        s_band_xmap[x] = (uint16_t)(((uint32_t)off * 256u) / 266u);
                        s_band_xweights[x].src_idx = w266[off].src_idx;
                        s_band_xweights[x].weight1 = w266[off].weight1;
                    }
                }
            }
        }
    } else {
        if (s_crop_sides == 2) {
            /* CROP_SIDES_8PX_FILL: Crop 8px sides and fill active width (240px from 8..247) */
            if (s_scale_mode == SCALE_INTEGER) {
                /* Centered 240x240 on 320x240: 40px borders on left and right */
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 40 || x >= 280) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        s_band_xmap[x] = (uint16_t)(8u + (x - 40u));
                        s_band_xweights[x].src_idx = s_band_xmap[x];
                    }
                }
            } else if (s_scale_mode == SCALE_FIT) {
                /* Proportional 8:7 PAR aspect ratio (~293x240 centered with 13px borders) */
                dmg_scale_weight_t w293[293];
                dmg_init_shimmerless_weights(240, 293, w293);
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 13 || x >= 306) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        uint16_t off = (uint16_t)(x - 13u);
                        s_band_xmap[x] = (uint16_t)(8u + (((uint32_t)off * 240u) / 293u));
                        s_band_xweights[x].src_idx = (uint16_t)(8u + w293[off].src_idx);
                        s_band_xweights[x].weight1 = w293[off].weight1;
                    }
                }
            } else {
                /* SCALE_STRETCH: Full screen 320x240 */
                dmg_scale_weight_t w320[320];
                dmg_init_shimmerless_weights(240, 320, w320);
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    s_band_xmap[x] = (uint16_t)(8u + (((uint32_t)x * 240u) / (uint32_t)ILI9341_WIDTH));
                    s_band_xweights[x].src_idx = (uint16_t)(8u + w320[x].src_idx);
                    s_band_xweights[x].weight1 = w320[x].weight1;
                }
            }
        } else {
            /* Normal 256 pixel native mapping (0..255) */
            if (s_scale_mode == SCALE_INTEGER) {
                /* Centered 256x240 on 320x240: 32px borders on left and right */
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 32 || x >= 288) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        s_band_xmap[x] = (uint16_t)(x - 32);
                        s_band_xweights[x].src_idx = s_band_xmap[x];
                    }
                }
            } else if (s_scale_mode == SCALE_FIT) {
                /* Proportional NES 8:7 PAR aspect ratio (~293x240 centered with 13px/14px borders) */
                dmg_scale_weight_t w293[293];
                dmg_init_shimmerless_weights(256, 293, w293);
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 13 || x >= 306) {
                        s_band_xmap[x] = 0xFFFFu; /* Black border */
                    } else {
                        uint16_t off = (uint16_t)(x - 13u);
                        s_band_xmap[x] = (uint16_t)(((uint32_t)off * 256u) / 293u);
                        s_band_xweights[x].src_idx = w293[off].src_idx;
                        s_band_xweights[x].weight1 = w293[off].weight1;
                    }
                }
            } else {
                /* SCALE_STRETCH: Full screen 320x240 */
                dmg_scale_weight_t w320[320];
                dmg_init_shimmerless_weights(256, 320, w320);
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    s_band_xmap[x] = (uint16_t)(((uint32_t)x * 256u) / (uint32_t)ILI9341_WIDTH);
                    s_band_xweights[x].src_idx = w320[x].src_idx;
                    s_band_xweights[x].weight1 = w320[x].weight1;
                }
            }
        }
    }
    s_band_xmap_ready = true;
}

static void nes_prepare_ymap(void) {
    if (s_ymap_ready) return;
    for (uint16_t y = 0; y < ILI9341_HEIGHT; y++) {
        if (s_height_fill && s_scale_mode != SCALE_INTEGER) {
            /* Scale active 224 lines (lines 8..231) to 240 display rows: 8 + (y * 224) / 240 */
            uint16_t src_y = 8u + (uint16_t)(((uint32_t)y * 224u) / 240u);
            if (src_y >= 240u) src_y = 239u;
            s_ymap[y] = (uint8_t)src_y;
        } else {
            /* 1:1 direct native scanline mapping */
            s_ymap[y] = (uint8_t)y;
        }
    }
    s_ymap_ready = true;
}

/* ST7789 power-on, after the hardware reset. Deliberately short: it is the
 * sequence PicoGBC's mk_st7789.c runs in the field on this console, and the
 * one pico-bootLoader uses too, so the two agree by construction rather than by
 * having been debugged separately. */
static void st7789_init_sequence(void) {
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    write_cmd(dmg_panel_get_invert() ? CMD_INVON : CMD_INVOFF);

    write_cmd(CMD_COLMOD);
    write_data(0x55); /* 16-bit RGB565 */

    apply_madctl();

    /* Physical panel timing: PORCTRL 0xB2 + FRCTRL2 0xC6 = 111 Hz.
     *
     * This belongs in the ST7789 branch and only here. The factory ST7789 sweep
     * is ~60 Hz (16.6 ms), the same order as the ~14 ms the Core 1 blitter takes
     * over SPI, so the GRAM write and the beam scan collide and the overlap shows
     * as a horizontal tear through fades and full-screen scrolling. At ~111 Hz
     * (~9 ms) the sweep finishes long before the next frame is written.
     * Values live in shared_menu/dmg_panel.c, one place for all nine emulators. */
    dmg_panel_st7789_timing(panel_timing_write);

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    write_cmd(CMD_NORON);
    sleep_ms(10);

    write_cmd(CMD_DISPON);
    sleep_ms(120);
}

void ili9341_init(void) {
    /* Panel and orientation come from /boot.txt via shared_menu, so this
     * driver no longer has its own idea of which display it is driving. Must
     * run before the sequences below, which read the resolved values. */
    dmg_panel_config_init();

    gpio_init(ILI9341_PIN_CS);
    gpio_set_dir(ILI9341_PIN_CS, GPIO_OUT);
    cs_high();

    gpio_init(ILI9341_PIN_DC);
    gpio_set_dir(ILI9341_PIN_DC, GPIO_OUT);
    dc_cmd();

    gpio_init(ILI9341_PIN_RST);
    gpio_set_dir(ILI9341_PIN_RST, GPIO_OUT);
    gpio_put(ILI9341_PIN_RST, 1);
    sleep_ms(5);

    gpio_init(ILI9341_PIN_BL);
    gpio_set_dir(ILI9341_PIN_BL, GPIO_OUT);
    ili9341_set_backlight(false);

    spi_init(ILI9341_SPI, SPI_INIT_BAUD);
    gpio_set_function(ILI9341_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(ILI9341_PIN_MOSI, GPIO_FUNC_SPI);
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    /* Hardware reset */
    gpio_put(ILI9341_PIN_RST, 0);
    sleep_ms(15);
    gpio_put(ILI9341_PIN_RST, 1);
    sleep_ms(120);

    if (dmg_panel_get() == DMG_PANEL_ST7789) {
        st7789_init_sequence();
    } else {
        ili9341_init_sequence();
    }

    /* Switch SPI to operational baud rate and initialize DMA */
    spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);
    ili9341_dma_init();

    nes_prepare_band_xmap();
    nes_prepare_ymap();
    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("PicoNES display: %s MADCTL=0x%02X (%s) %s (%s) @ %u Hz (Progressive 60fps DMA streaming)\n",
           dmg_panel_get() == DMG_PANEL_ST7789 ? "ST7789" : "ILI9341",
           (unsigned)dmg_panel_get_madctl(),
           dmg_panel_has_explicit_madctl() ? "from boot.txt" : "from panel table",
           dmg_panel_get_invert() ? "INVON" : "INVOFF",
           dmg_panel_source(),
           spi_get_baudrate(ILI9341_SPI));
}

/* ILI9341 power-on: the recipe this driver has always run, unchanged apart
 * from MADCTL now coming from shared_menu. */
static void ili9341_init_sequence(void) {
    /* Software reset */
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    /* Display configuration matching verified pico-bootLoader profile */
    write_cmd(CMD_PWCTR1);
    write_data(0x23);

    write_cmd(CMD_PWCTR2);
    write_data(0x10);

    write_cmd(CMD_VMCTR1);
    write_data(0x3E);
    write_data(0x28);

    write_cmd(CMD_VMCTR2);
    write_data(0x86);

    ili9341_set_rotation(1); /* Landscape (320x240, 0x28) */

    write_cmd(CMD_COLMOD);
    write_data(0x55); /* 16-bit RGB565 */

    /* ILI9341 Frame Rate Control (FRMCTR1 0xB1): ~100 Hz */
    write_cmd(CMD_FRMCTR1);
    write_data(0x00);
    write_data(0x10);

    /* NOTE: the ST7789 refresh overclock (PORCTRL 0xB2 + FRCTRL2 0xC6 = 111 Hz)
     * used to live here. It was in the wrong branch: this function only runs on
     * an ILI9341, and 0xB2/0xC6 are ST7789-only registers that the ILI9341 does
     * not have. The effect was invisible at boot -- the console came up fine on
     * both panels -- and the Core2350B simply never got the overclock, sweeping
     * at the factory 60 Hz and tearing during full-screen transitions.
     * It is now in st7789_init_sequence(), through the shared helper. */

    write_cmd(CMD_DFUNCTR);
    write_data(0x08);
    write_data(0x82);
    write_data(0x27);

    write_cmd(CMD_GAMSET);
    write_data(0x01);

    write_cmd(CMD_INVCTR);
    write_data(0x00);

    write_cmd(CMD_PWCTR3);
    write_data(0x0A);
    write_data(0x00);

    write_cmd(CMD_PWCTR4);
    write_data(0x8A);
    write_data(0x2A);

    write_cmd(CMD_PWCTR5);
    write_data(0x8A);
    write_data(0xEE);

    write_cmd(CMD_GMCTRP1);
    static const uint8_t gamma_p[] = {
        0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
        0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00
    };
    write_data_buf(gamma_p, sizeof(gamma_p));

    write_cmd(CMD_GMCTRN1);
    static const uint8_t gamma_n[] = {
        0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
        0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F
    };
    write_data_buf(gamma_n, sizeof(gamma_n));

    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    write_cmd(CMD_DISPON);
    sleep_ms(120);
}

uint16_t *ili9341_get_framebuffer(void) {
    return s_fb;
}

uint16_t *ili9341_get_draw_buffer(void) {
    return s_fb;
}

/* Envia uma scanline no modo Interleave com janela ativa e DMA */
static void __not_in_flash_func(ili9341_stream_row)(const uint8_t *packed, uint16_t row, uint16_t win_x0, uint16_t win_x1, uint16_t win_w) {
    if (packed == NULL || row >= ILI9341_HEIGHT) return;

    cs_low();

    /* 1. CMD_CASET */
    dc_cmd();
    uint8_t cmd_caset = CMD_CASET;
    spi_write_blocking(ILI9341_SPI, &cmd_caset, 1);
    dc_data();
    uint8_t caset[4] = {
        (uint8_t)(win_x0 >> 8), (uint8_t)(win_x0 & 0xFF),
        (uint8_t)(win_x1 >> 8), (uint8_t)(win_x1 & 0xFF)
    };
    spi_write_blocking(ILI9341_SPI, caset, 4);

    /* 2. CMD_PASET */
    dc_cmd();
    uint8_t cmd_paset = CMD_PASET;
    spi_write_blocking(ILI9341_SPI, &cmd_paset, 1);
    dc_data();
    uint8_t raset[4] = {
        (uint8_t)(row >> 8), (uint8_t)(row & 0xFF),
        (uint8_t)(row >> 8), (uint8_t)(row & 0xFF)
    };
    spi_write_blocking(ILI9341_SPI, raset, 4);

    /* 3. CMD_RAMWR */
    dc_cmd();
    uint8_t cmd_ramwr = CMD_RAMWR;
    spi_write_blocking(ILI9341_SPI, &cmd_ramwr, 1);

    /* 4. Pixels RGB565 enviados em 8-bit contínuo (win_w * 2 bytes) via DMA */
    dc_data();
    size_t row_bytes = (size_t)win_w * 2u;
    if (s_spi_dma_chan >= 0) {
        dma_channel_transfer_from_buffer_now(s_spi_dma_chan, packed, row_bytes);
        dma_channel_wait_for_finish_blocking(s_spi_dma_chan);
    } else {
        spi_write_blocking(ILI9341_SPI, packed, row_bytes);
    }
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
}

static inline void nes_render_line_packed(uint8_t *line_packed, const uint16_t *src256, uint16_t row, uint16_t win_x0, uint16_t win_w, bool dim, bool shimmerless) {
    uint16_t *out16 = (uint16_t *)line_packed;
    if (shimmerless) {
        if (!dim) {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_band_xweights[x].src_idx;
                uint8_t w = s_band_xweights[x].weight1;
                uint16_t c;
                if (map_idx == 0xFFFFu) {
                    c = 0x0000;
                } else if (w == 0u || map_idx >= (NES_NATIVE_WIDTH - 1u)) {
                    c = rgb555_to_rgb565_fast(src256[map_idx]);
                } else {
                    c = rgb555_to_rgb565_fast(dmg_blend_rgb555(src256[map_idx], src256[map_idx + 1u], w));
                }
                out16[i] = __builtin_bswap16(c);
            }
        } else {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_band_xweights[x].src_idx;
                uint8_t w = s_band_xweights[x].weight1;
                uint16_t c;
                if (map_idx == 0xFFFFu) {
                    c = 0x0000;
                } else if (w == 0u || map_idx >= (NES_NATIVE_WIDTH - 1u)) {
                    c = rgb555_to_rgb565_fast(src256[map_idx]);
                } else {
                    c = rgb555_to_rgb565_fast(dmg_blend_rgb555(src256[map_idx], src256[map_idx + 1u], w));
                }
                c = (c >> 1) & 0x7BEFu; /* 50% CRT scanline attenuation */
                out16[i] = __builtin_bswap16(c);
            }
        }
    } else {
        if (!dim) {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_band_xmap[x];
                uint16_t c = (map_idx == 0xFFFFu) ? 0x0000 : rgb555_to_rgb565_fast(src256[map_idx]);
                out16[i] = __builtin_bswap16(c);
            }
        } else {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_band_xmap[x];
                uint16_t c = (map_idx == 0xFFFFu) ? 0x0000 : rgb555_to_rgb565_fast(src256[map_idx]);
                c = (c >> 1) & 0x7BEFu; /* 50% CRT scanline attenuation */
                out16[i] = __builtin_bswap16(c);
            }
        }
    }

    /* Volume OSD HUD overlay */
    int hud_x, hud_w;
    const uint16_t *hud = dmg_volume_hud_get_row(row, &hud_x, &hud_w);
    if (hud) {
        for (int hx = 0; hx < hud_w; hx++) {
            uint16_t c = hud[hx];
            if (c != 0x0001) {
                int target_x = (hud_x + hx) - (int)win_x0;
                if (target_x >= 0 && target_x < (int)win_w) {
                    out16[target_x] = __builtin_bswap16(c);
                }
            }
        }
    }

    /* Low-battery overlay (top-right). Only the part inside the active window
     * lands here; nes_push_batt_overlay_borders() sends the rest. */
    int bov_x, bov_w;
    const uint16_t *bov = dmg_batt_overlay_get_row(row, &bov_x, &bov_w);
    if (bov) {
        for (int bx = 0; bx < bov_w; bx++) {
            uint16_t c = bov[bx];
            if (c != 0x0001) {
                int target_x = (bov_x + bx) - (int)win_x0;
                if (target_x >= 0 && target_x < (int)win_w) {
                    out16[target_x] = __builtin_bswap16(c);
                }
            }
        }
    }
}

/* Part of the low-battery overlay that falls outside the active window (Fit and
 * Integer crop the sent width, e.g. Integer ends at x=287 while the overlay sits
 * at ~247..310). Those border columns are never rewritten by the game, so this
 * runs after each frame: the overlay over black while it is shown, and one black
 * pass when it hides so nothing is left behind. Up to 64x14 px, ~0.2 ms. */
static bool s_bov_borders_drawn = false;

static void __not_in_flash_func(nes_push_bov_span)(int x0, int x1, int bov_x, bool visible) {
    if (x1 <= x0) return;
    const uint16_t y0 = dmg_batt_overlay_y0();
    const uint16_t h  = dmg_batt_overlay_h();
    set_addr_window((uint16_t)x0, y0, (uint16_t)(x1 - 1), (uint16_t)(y0 + h - 1));
    dc_cmd();
    cs_low();
    uint8_t cmd_ramwr = CMD_RAMWR;
    spi_write_blocking(ILI9341_SPI, &cmd_ramwr, 1);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    dc_data();
    uint16_t line[64];
    for (uint16_t r = 0; r < h; r++) {
        const uint16_t *src = visible ? dmg_batt_overlay_get_row((uint16_t)(y0 + r), NULL, NULL) : NULL;
        int n = 0;
        for (int x = x0; x < x1 && n < 64; x++, n++) {
            uint16_t c = src ? src[x - bov_x] : 0x0000u;
            if (c == 0x0001u) c = 0x0000u;   /* transparent over the black border */
            line[n] = __builtin_bswap16(c);
        }
        spi_write_blocking(ILI9341_SPI, (const uint8_t *)line, (size_t)n * 2u);
    }
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    cs_high();
}

static void __not_in_flash_func(nes_push_batt_overlay_borders)(uint16_t win_x0, uint16_t win_w) {
    int bov_x, bov_w;
    const bool visible = dmg_batt_overlay_visible() &&
                         dmg_batt_overlay_get_row(dmg_batt_overlay_y0(), &bov_x, &bov_w) != NULL;
    static int s_last_x = 0, s_last_w = 0;
    if (!visible) {
        if (!s_bov_borders_drawn) return;
        /* One black pass over where it was last drawn. */
        bov_x = s_last_x;
        bov_w = s_last_w;
    }
    const int ox0 = bov_x, ox1 = bov_x + bov_w;
    const int wx0 = (int)win_x0, wx1 = (int)win_x0 + (int)win_w;
    /* left of the window, then right of it */
    nes_push_bov_span(ox0, (ox1 < wx0 ? ox1 : wx0), bov_x, visible);
    nes_push_bov_span((ox0 > wx1 ? ox0 : wx1), ox1, bov_x, visible);
    s_bov_borders_drawn = visible;
    s_last_x = bov_x;
    s_last_w = bov_w;
}

/* Core 1 background display worker (desacoplado do Core 0) */
static void __not_in_flash_func(nes_blitter_core1)(void) {
    while (1) {
        if (!s_blitter_active) {
            tight_loop_contents();
            continue;
        }

        if (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
            const uint16_t *field_data = s_nes_buf[s_render_buf_idx];
            const uint8_t parity = s_render_field_parity;
            const uint8_t *ymap = s_ymap;
            const bool scanlines = s_scanlines_enabled;
            const bool interleave = s_nes_interleave;
            const bool shimmerless = s_shimmerless_enabled && (s_scale_mode != SCALE_INTEGER);

            uint16_t win_x0, win_x1, win_w;
            get_active_window(&win_x0, &win_x1, &win_w);
            size_t line_bytes = (size_t)win_w * 2u;

            if (!interleave) {
                /* Progressive mode: full active window with ping-pong DMA */
                set_addr_window(win_x0, 0, win_x1, ILI9341_HEIGHT - 1);
                cs_low();
                dc_cmd();
                uint8_t cmd_ramwr = CMD_RAMWR;
                spi_write_blocking(ILI9341_SPI, &cmd_ramwr, 1);
                while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
                dc_data();

                /* Line 0: render into ping-pong buffer 0 */
                uint8_t src_y0 = ymap[0];
                const uint16_t *src256_0 = &field_data[src_y0 * NES_NATIVE_WIDTH];
                nes_render_line_packed(s_dma_line_buf[0], src256_0, 0, win_x0, win_w, false, shimmerless);

                /* Launch DMA for line 0 */
                if (s_spi_dma_chan >= 0) {
                    dma_channel_transfer_from_buffer_now(s_spi_dma_chan, s_dma_line_buf[0], line_bytes);
                } else {
                    spi_write_blocking(ILI9341_SPI, s_dma_line_buf[0], line_bytes);
                }

                /* Lines 1..239: parallel CPU render + DMA transfer */
                for (uint16_t i = 1; i < (uint16_t)ILI9341_HEIGHT; i++) {
                    uint8_t cur_buf_idx = (uint8_t)(i & 1u);
                    uint8_t *cur_buf = s_dma_line_buf[cur_buf_idx];

                    /* Compute line i while line i-1 is being transferred by DMA */
                    uint8_t src_y = ymap[i];
                    const uint16_t *src256 = &field_data[src_y * NES_NATIVE_WIDTH];
                    const bool dim = scanlines && (i & 1u);

                    nes_render_line_packed(cur_buf, src256, i, win_x0, win_w, dim, shimmerless);

                    /* Wait for DMA transfer of line i-1 to complete */
                    if (s_spi_dma_chan >= 0) {
                        dma_channel_wait_for_finish_blocking(s_spi_dma_chan);
                        dma_channel_transfer_from_buffer_now(s_spi_dma_chan, cur_buf, line_bytes);
                    } else {
                        spi_write_blocking(ILI9341_SPI, cur_buf, line_bytes);
                    }
                }

                /* Wait for line 239 DMA transfer to finish */
                if (s_spi_dma_chan >= 0) {
                    dma_channel_wait_for_finish_blocking(s_spi_dma_chan);
                }

                while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
                while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
                cs_high();
            } else {
                /* Interleaved mode: alternating lines (120 lines/frame) */
                for (uint16_t i = 0; i < 120u; i++) {
                    uint16_t actual_row = (uint16_t)((i << 1) | parity);
                    uint8_t src_y = ymap[actual_row];
                    const uint16_t *src256 = &field_data[src_y * NES_NATIVE_WIDTH];
                    const bool dim = scanlines && (actual_row & 1u);

                    nes_render_line_packed(s_dma_line_buf[0], src256, actual_row, win_x0, win_w, dim, shimmerless);

                    ili9341_stream_row(s_dma_line_buf[0], actual_row, win_x0, win_x1, win_w);
                }
            }

            nes_push_batt_overlay_borders(win_x0, win_w);

            __atomic_store_n(&s_field_ready, false, __ATOMIC_RELEASE);
        } else {
            tight_loop_contents();
        }
    }
}

/* Copia scanline para o buffer duplo sem bloqueio */
void ili9341_stream_nes_line(int line, const uint16_t *rgb555_256) {
    if (!s_blitter_active || line < 0 || line >= (int)NES_NATIVE_HEIGHT) return;

    if (rgb555_256 != NULL) {
        memcpy(&s_nes_buf[s_write_buf_idx][line * NES_NATIVE_WIDTH],
               rgb555_256,
               NES_NATIVE_WIDTH * sizeof(uint16_t));
    }
}

void ili9341_stream_nes_frame_end(void) {
    if (!s_blitter_active) return;

    /* Amostra cor de backdrop da paleta atual do InfoNES */
    s_current_backdrop555 = PalTable[0];

    /* Se o Core 1 ainda estiver renderizando o frame anterior, aguarda liberação */
    while (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    /* Submete o novo buffer para o Core 1 */
    s_render_buf_idx = s_write_buf_idx;
    s_render_field_parity = s_field_parity;
    __atomic_store_n(&s_field_ready, true, __ATOMIC_RELEASE);

    /* Alterna buffer e paridade para o próximo frame */
    s_write_buf_idx ^= 1u;
    s_field_parity ^= 1u;
}

void ili9341_blitter_start(void) {
    if (s_blitter_active) return;

    /* Launch Core 1 blitter once on the first invocation */
    if (!s_blitter_core1_launched) {
        s_blitter_core1_launched = true;
        multicore_launch_core1(nes_blitter_core1);
    }

    nes_prepare_band_xmap();
    nes_prepare_ymap();
    ili9341_fill_screen(0x0000);
    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    s_write_buf_idx = 0u;
    s_render_buf_idx = 0u;
    s_field_parity = 0u;
    s_render_field_parity = 0u;
    s_current_backdrop555 = PalTable[0];
    memset(s_nes_buf, 0, sizeof(s_nes_buf));
    __atomic_store_n(&s_field_ready, false, __ATOMIC_RELEASE);
    s_blitter_active = true;
}

void ili9341_blitter_stop(void) {
    if (!s_blitter_active) return;

    /* 1. Wait with safety timeout (50ms) for Core 1 to finish active field */
    uint32_t t0 = time_us_32();
    while (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
        if ((time_us_32() - t0) > 50000u) break;
        tight_loop_contents();
    }

    /* 2. Wait for any active DMA transfer to complete */
    if (s_spi_dma_chan >= 0) {
        dma_channel_wait_for_finish_blocking(s_spi_dma_chan);
    }

    /* 3. Wait for SPI engine to complete */
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    /* 4. Deselect ILI9341 */
    cs_high();

    /* 5. Put Core 1 into passive wait without resetting hardware */
    s_blitter_active = false;
    __atomic_store_n(&s_field_ready, false, __ATOMIC_RELEASE);
}

void ili9341_set_interleave(bool enable) {
    s_nes_interleave = enable;
}

bool ili9341_get_interleave(void) {
    return s_nes_interleave;
}

bool ili9341_is_active_nes_line(int line) {
    if (!s_nes_interleave) return true;
    return ((line & 1u) == s_field_parity);
}

void ili9341_set_scanlines(bool enable) {
    s_scanlines_enabled = enable;
}

bool ili9341_get_scanlines(void) {
    return s_scanlines_enabled;
}

void ili9341_set_shimmerless(bool enable) {
    s_shimmerless_enabled = enable;
}

bool ili9341_get_shimmerless(void) {
    return s_shimmerless_enabled;
}

void ili9341_set_scale_mode(ili9341_scale_mode_t mode) {
    if (s_scale_mode != mode) {
        s_scale_mode = mode;
        s_band_xmap_ready = false;
        nes_prepare_band_xmap();
        s_ymap_ready = false;
        nes_prepare_ymap();
        if (s_blitter_active) {
            ili9341_fill_screen(0x0000);
        }
    }
}

ili9341_scale_mode_t ili9341_get_scale_mode(void) {
    return s_scale_mode;
}

void ili9341_set_height_fill(bool enable) {
    if (s_height_fill != enable) {
        s_height_fill = enable;
        s_ymap_ready = false;
        nes_prepare_ymap();
    }
}

bool ili9341_get_height_fill(void) {
    return s_height_fill;
}

void ili9341_set_crop_sides(uint8_t mode) {
    if (s_crop_sides != mode) {
        s_crop_sides = mode;
        s_band_xmap_ready = false;
        nes_prepare_band_xmap();
        if (s_blitter_active) {
            ili9341_fill_screen(0x0000);
        }
    }
}

uint8_t ili9341_get_crop_sides(void) {
    return s_crop_sides;
}

static inline char get_font_slice(char c, int row) {
    if (c < 32 || c >= (32 + 95)) return 0;
    return font_8x8[(c - 32) + row * 95];
}

void ili9341_draw_char(char c, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg) {
    if (x + 8 > ILI9341_WIDTH || y + 8 > ILI9341_HEIGHT) return;
    for (int r = 0; r < 8; r++) {
        char slice = get_font_slice(c, r);
        uint16_t *line = &s_fb[(y + r) * ILI9341_WIDTH + x];
        for (int col = 0; col < 8; col++) {
            line[col] = (slice & (1 << col)) ? fg : bg;
        }
    }
}

void ili9341_text(const char *str, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg) {
    if (!str) return;
    uint16_t cx = x;
    while (*str && cx + 8 <= ILI9341_WIDTH) {
        ili9341_draw_char(*str, cx, y, fg, bg);
        cx += 8;
        str++;
    }
}

void ili9341_core1_start(void) {
    /* Kept for API compatibility; blitter starts explicitly on game enter */
}

void ili9341_submit_frame(void) {
    ili9341_stream_nes_frame_end();
}

void ili9341_wait_frame_done(void) {
    if (!s_blitter_active) return;
    while (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }
}

bool ili9341_is_frame_busy(void) {
    return __atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE);
}

/* Menu synchronous flush (flushes full 320x240 s_fb with RGB555->RGB565 conversion in 8-bit SPI) */
void ili9341_flush(bool wait) {
    (void)wait;
    ili9341_wait_frame_done();

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();

    static uint8_t line_packed[ILI9341_WIDTH * 2u];
    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        const uint16_t *src = &s_fb[y * ILI9341_WIDTH];
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t c = rgb555_to_rgb565(src[x]);
            line_packed[2 * x + 0] = (uint8_t)(c >> 8);
            line_packed[2 * x + 1] = (uint8_t)(c & 0xFF);
        }
        spi_write_blocking(ILI9341_SPI, line_packed, ILI9341_WIDTH * 2u);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
}

void ili9341_flush_buffer(const uint16_t *buffer, bool wait) {
    (void)wait;
    ili9341_wait_frame_done();

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();

    static uint8_t line_packed[ILI9341_WIDTH * 2u];
    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        const uint16_t *src = &buffer[y * ILI9341_WIDTH];
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t c = rgb555_to_rgb565(src[x]);
            line_packed[2 * x + 0] = (uint8_t)(c >> 8);
            line_packed[2 * x + 1] = (uint8_t)(c & 0xFF);
        }
        spi_write_blocking(ILI9341_SPI, line_packed, ILI9341_WIDTH * 2u);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
}

void ili9341_flush_snes_32bpp(const uint32_t *pixels, int w, int h, int pitch_bytes) {
    if (!pixels || w <= 0 || h <= 0) return;
    ili9341_wait_frame_done();

    int x0 = (ILI9341_WIDTH - w) / 2;
    int y0 = (ILI9341_HEIGHT - h) / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;

    set_addr_window((uint16_t)x0, (uint16_t)y0, (uint16_t)(x0 + w - 1), (uint16_t)(y0 + h - 1));
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();

    static uint8_t line_packed[320 * 2u];
    for (int y = 0; y < h; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)pixels + y * pitch_bytes);
        for (int x = 0; x < w; x++) {
            uint32_t c = src[x];
            // 0x00RRGGBB -> RGB565 (R:5, G:6, B:5)
            uint16_t r = (uint16_t)((c >> 19) & 0x1F);
            uint16_t g = (uint16_t)((c >> 10) & 0x3F);
            uint16_t b = (uint16_t)((c >> 3)  & 0x1F);
            uint16_t rgb565 = (uint16_t)((r << 11) | (g << 5) | b);
            line_packed[2 * x + 0] = (uint8_t)(rgb565 >> 8);
            line_packed[2 * x + 1] = (uint8_t)(rgb565 & 0xFF);
        }
        spi_write_blocking(ILI9341_SPI, line_packed, (size_t)(w * 2));
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
}

void ili9341_flush_rows(int y_start, int y_end) {
    if (y_start < 0) y_start = 0;
    if (y_end >= ILI9341_HEIGHT) y_end = ILI9341_HEIGHT - 1;
    if (y_start > y_end) return;

    ili9341_wait_frame_done();

    set_addr_window(0, (uint16_t)y_start, ILI9341_WIDTH - 1, (uint16_t)y_end);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();

    static uint8_t line_packed[ILI9341_WIDTH * 2u];
    for (int y = y_start; y <= y_end; y++) {
        const uint16_t *src = &s_fb[y * ILI9341_WIDTH];
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t c = rgb555_to_rgb565(src[x]);
            line_packed[2 * x + 0] = (uint8_t)(c >> 8);
            line_packed[2 * x + 1] = (uint8_t)(c & 0xFF);
        }
        spi_write_blocking(ILI9341_SPI, line_packed, ILI9341_WIDTH * 2u);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
}

/* Direct-to-panel block write, host-order RGB565, no framebuffer. Used by the
 * standby-resume intro on core 1 (shared_menu/dmg_boot_intro.c) while core 0
 * loads the game -- so it must not touch s_fb or the blitter state, only the
 * SPI. Blocking SPI on purpose: the DMA channel belongs to the blitter. */
void ili9341_write_rect_565(int x, int y, int w, int h, const uint16_t *px) {
    if (px == NULL || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > ILI9341_WIDTH || y + h > ILI9341_HEIGHT) return;

    set_addr_window((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    write_cmd(CMD_RAMWR);
    dc_data();
    cs_low();
    uint8_t buf[64];
    size_t n = 0;
    const int count = w * h;
    for (int i = 0; i < count; i++) {
        buf[n++] = (uint8_t)(px[i] >> 8);
        buf[n++] = (uint8_t)(px[i] & 0xFF);
        if (n == sizeof(buf)) {
            spi_write_blocking(ILI9341_SPI, buf, n);
            n = 0;
        }
    }
    if (n) spi_write_blocking(ILI9341_SPI, buf, n);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    cs_high();
}

void ili9341_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ILI9341_WIDTH)  w = ILI9341_WIDTH - x;
    if (y + h > ILI9341_HEIGHT) h = ILI9341_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int r = y; r < y + h; r++) {
        uint16_t *line = &s_fb[r * ILI9341_WIDTH + x];
        for (int c = 0; c < w; c++) {
            line[c] = color;
        }
    }
}

void ili9341_fill_screen(uint16_t color) {
    for (int i = 0; i < ILI9341_WIDTH * ILI9341_HEIGHT; i++) {
        s_fb[i] = color;
    }
    ili9341_flush(true);
}
