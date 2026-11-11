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
__attribute__((weak)) uint16_t PalTable[64] = {0};

/* ST7789 power-on sequence, identical to the PicoSMS and PicoNES drivers on
 * the same panel. Declared before ili9341_init() and defined after it, so the
 * GPIO/SPI bring-up stays in one place. */
static void st7789_init_sequence(void);

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
#define SPI_RUN_BAUD   72000000u

/* Dedicated DMA channel for Core 1 SPI scanline transmission */
static int s_spi_dma_chan = -1;
static dma_channel_config s_spi_dma_cfg;

static void ili9341_dma_init(void) {
    if (s_spi_dma_chan < 0) {
        s_spi_dma_chan = dma_claim_unused_channel(false);
        if (s_spi_dma_chan >= 0) {
            s_spi_dma_cfg = dma_channel_get_default_config(s_spi_dma_chan);
            channel_config_set_transfer_data_size(&s_spi_dma_cfg, DMA_SIZE_8);
            channel_config_set_dreq(&s_spi_dma_cfg, spi_get_dreq(ILI9341_SPI, true));
            channel_config_set_read_increment(&s_spi_dma_cfg, true);
            channel_config_set_write_increment(&s_spi_dma_cfg, false);
        }
    }
}

/* Framebuffer used by the menu system (320x240 RGB555) */
static uint16_t __aligned(4) s_fb[ILI9341_WIDTH * ILI9341_HEIGHT];

/* ------------------------------------------------------------------ */
/* Per-scanline streaming blitter state (Dual-Core PicoDMG strategy)   */
/* ------------------------------------------------------------------ */
#define NES_LCD_SLOTS 128u
#define NES_ROW_BYTES (ILI9341_WIDTH * 2u)

static uint8_t __aligned(4) s_slot_data[NES_LCD_SLOTS][NES_ROW_BYTES];
static uint8_t s_slot_row[NES_LCD_SLOTS];
static volatile uint8_t s_slot_state[NES_LCD_SLOTS]; /* 1 = ready for Core 1 */
static volatile uint8_t s_write_slot = 0u;
static volatile uint8_t s_read_slot = 0u;
static volatile bool s_blitter_core1_launched = false;
static volatile bool s_blitter_active = false;
static volatile bool s_scanlines_enabled = false;
static ili9341_scale_mode_t s_scale_mode = SCALE_FIT;
static ili9341_task_fn s_background_task = NULL;

void ili9341_set_background_task(ili9341_task_fn task) {
    s_background_task = task;
}

static int16_t s_last_streamed_row = -1;
static bool s_md_interleave = true;

#if GWSND_OFFLOAD
extern void gwsnd_bridge_drain(void);
#endif

static uint8_t s_next_row = 0u;
static uint8_t s_field_parity = 0u; /* 0 or 1: interlaced fields to halve SPI bandwidth */
static uint32_t s_row_hash[ILI9341_HEIGHT];
static bool s_bov_frame_rows = false;   /* low-battery overlay: rows emitted this frame */
static uint16_t s_band_xmap[ILI9341_WIDTH];
static bool s_band_xmap_ready = false;
static uint16_t s_backdrop_row[256];
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

static void set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
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

void ili9341_set_rotation(uint8_t rotation) {
    /* The rotation index the project uses (1 = landscape, 3 = landscape
     * inverted) is kept for callers that only care about the two landscape
     * orientations, but the MADCTL itself now comes from shared_menu, which
     * knows which panel is attached. The ST7789's landscape value is 0x60, not
     * the ILI9341's 0x28, and the 180-degree case differs too (0xA0 vs 0xE8), so
     * keeping the hardcoded ILI9341 table would mis-orient the console panel.
     * That table lives in dmg_panel.c, measured on the hardware. */
    (void)rotation;
    write_cmd(CMD_MADCTL);
    write_data(dmg_panel_get_madctl());
}

/* Converts 15-bit RGB555 (InfoNES native format with bit 15 backdrop flag) to ILI9341 16-bit RGB565 */
static inline uint16_t rgb555_to_rgb565(uint16_t p) {
    uint16_t c = p & 0x7FFF;
    uint16_t r = (c >> 10) & 0x1F;
    uint16_t g = (c >> 5) & 0x1F;
    uint16_t b = c & 0x1F;
    uint16_t g6 = (g << 1) | (g >> 4);
    return (uint16_t)((r << 11) | (g6 << 5) | b);
}

static void nes_prepare_band_xmap(void) {
    if (s_band_xmap_ready) return;
    if (dmg_is_gb_mode()) {
        if (s_scale_mode == SCALE_INTEGER) {
            /* Centered 256px on 266px window (x=45..310): 5px borders inside window */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 50 || x >= 306) {
                    s_band_xmap[x] = 0xFFFFu; /* Black border */
                } else {
                    s_band_xmap[x] = (uint16_t)(x - 50u);
                }
            }
        } else {
            /* SCALE_FIT & SCALE_STRETCH: Scale 256 across 266px window */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 45 || x >= 311) {
                    s_band_xmap[x] = 0xFFFFu; /* Black border */
                } else {
                    s_band_xmap[x] = (uint16_t)(((uint32_t)(x - 45) * 256u) / 266u);
                }
            }
        }
    } else {
        if (s_scale_mode == SCALE_INTEGER) {
            /* Centered 256x240 on 320x240: 32px borders on left and right */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 32 || x >= 288) {
                    s_band_xmap[x] = 0xFFFFu; /* Black border */
                } else {
                    s_band_xmap[x] = (uint16_t)(x - 32);
                }
            }
        } else if (s_scale_mode == SCALE_FIT) {
            /* Proportional NES 8:7 PAR aspect ratio (~293x240 centered with 13px/14px borders) */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 13 || x >= 306) {
                    s_band_xmap[x] = 0xFFFFu; /* Black border */
                } else {
                    s_band_xmap[x] = (uint16_t)(((uint32_t)(x - 13) * 256u) / 293u);
                }
            }
        } else {
            /* SCALE_STRETCH: Full screen 320x240 */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                s_band_xmap[x] = (uint16_t)(((uint32_t)x * 256u) / (uint32_t)ILI9341_WIDTH);
            }
        }
    }
    s_band_xmap_ready = true;
}

void ili9341_init(void) {
    /* Panel and orientation come from /boot.txt via shared_menu, so this driver
     * no longer has its own idea of which display it is driving. Must run before
     * the sequence below, which reads the resolved values. */
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

    /* Software reset */
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    /* The ST7789 on the Core2350B console does not take the ILI9341 power
     * sequence: PWCTR1..5, VMCTR1/2, FRMCTR1 and DFUNCTR are ILI9341-only
     * registers, and the ST7789 is a MIPI DBI panel with its own addressing, so
     * writing the ILI9341 sequence to it gave a blank or mis-scanned screen.
     * The two are therefore kept apart. */
    if (dmg_panel_get() == DMG_PANEL_ST7789) {
        st7789_init_sequence();
        spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);
        printf("PicoMD display: ST7789 MADCTL=0x%02X (%s) %s @ %u Hz\n",
               (unsigned)dmg_panel_get_madctl(),
               dmg_panel_has_explicit_madctl() ? "from boot.txt" : "from panel table",
               dmg_panel_get_invert() ? "INVON" : "INVOFF",
               spi_get_baudrate(ILI9341_SPI));
        return;
    }

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

    write_cmd(CMD_FRMCTR1);
    write_data(0x00);
    write_data(0x18); /* ~79 Hz frame rate */

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

    /* Switch SPI to operational baud rate */
    spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);
    ili9341_dma_init();

    nes_prepare_band_xmap();
    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("ILI9341: initialized in landscape 320x240 @ %u Hz (Interlaced Core 1 streaming, DMA %s)\n",
           spi_get_baudrate(ILI9341_SPI), (s_spi_dma_chan >= 0) ? "ON" : "OFF");
}

/* ST7789 power-on. Deliberately a short sequence: SWRESET, SLPOUT, inversion,
 * COLMOD, MADCTL, NORON, DISPON, in that order. The ILI9341 power registers are
 * absent on purpose -- on a MIPI DBI panel they either do not exist or address
 * unrelated internal state.
 *
 * The delays are the datasheet minimums, not padding: NORON needs 10 ms before
 * the next command, DISPON needs 120 ms before the panel is usable. */
/* Adapter for dmg_panel_st7789_timing(), which takes one combined
 * command+parameter primitive, while this driver keeps write_cmd() and
 * write_data_buf() separate. */
static void panel_timing_write(uint8_t cmd, const uint8_t *data, size_t len) {
    write_cmd(cmd);
    if (data != NULL && len > 0) write_data_buf(data, len);
}

static void st7789_init_sequence(void) {
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    write_cmd(dmg_panel_get_invert() ? CMD_INVON : CMD_INVOFF);

    write_cmd(CMD_COLMOD);
    write_data(0x55); /* 16-bit RGB565 */

    /* Orientation from the panel table, not hardcoded: 0x28 for the ILI9341 and
     * 0x60 for the ST7789, the same value plus MX. */
    write_cmd(CMD_MADCTL);
    write_data(dmg_panel_get_madctl());

    /* Physical panel timing: PORCTRL 0xB2 + FRCTRL2 0xC6 = 111 Hz.
     *
     * The factory ST7789 sweep is ~60 Hz (16.6 ms), the same order as the ~14 ms
     * the Core 1 blitter takes over SPI, so the GRAM write and the beam scan
     * collide and the overlap shows as a horizontal tear through fades and
     * full-screen scrolling. At ~111 Hz (~9 ms) the sweep finishes long before
     * the next frame is written.
     *
     * Values live in shared_menu/dmg_panel.c, so a panel timing fix is applied
     * once for all nine emulators instead of one driver at a time. */
    dmg_panel_st7789_timing(panel_timing_write);

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    write_cmd(CMD_NORON);
    sleep_ms(10);

    write_cmd(CMD_DISPON);
    sleep_ms(120);
}

void ili9341_clear_black(void) {
    ili9341_wait_frame_done();

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    static const uint16_t zero_line[ILI9341_WIDTH] = {0};
    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        spi_write16_blocking(ILI9341_SPI, zero_line, ILI9341_WIDTH);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    memset(s_fb, 0, sizeof(s_fb));
}

static volatile bool s_fps_enabled = false;
static char s_fps_text[16] = "60";

void ili9341_set_fps_overlay(bool enable, const char *fps_str) {
    if (s_fps_enabled != enable) {
        s_fps_enabled = enable;
        for (int r = 0; r < ILI9341_HEIGHT; r++) {
            s_row_hash[r] = 0xFFFFFFFFu;
        }
    }
    if (fps_str && fps_str[0] != '\0') {
        if (strncmp(s_fps_text, fps_str, sizeof(s_fps_text) - 1) != 0) {
            strncpy(s_fps_text, fps_str, sizeof(s_fps_text) - 1);
            s_fps_text[sizeof(s_fps_text) - 1] = '\0';
            for (int r = 0; r < ILI9341_HEIGHT; r++) {
                s_row_hash[r] = 0xFFFFFFFFu;
            }
        }
    }
}

bool ili9341_get_fps_overlay(void) {
    return s_fps_enabled;
}

uint16_t *ili9341_get_framebuffer(void) {
    return s_fb;
}

uint16_t *ili9341_get_draw_buffer(void) {
    return s_fb;
}

/* Sends a scanline to the ILI9341 display with hardware DMA and parallel Core 1 task execution */
static void __not_in_flash_func(ili9341_stream_row)(const uint8_t *packed, uint16_t row) {
    if (packed == NULL || row >= ILI9341_HEIGHT) return;

    /* Continuous contiguous scanline optimization (Progressive mode):
     * If this row immediately follows the last streamed row and CS is active (s_last_streamed_row >= 0),
     * the display RAM write pointer is already positioned at pixel (0, row).
     * We don't need PASET or RAMWR or CS toggle, just stream the pixel data! */
    bool contiguous = (s_last_streamed_row >= 0) && (row == (uint16_t)(s_last_streamed_row + 1));

    if (!contiguous) {
        if (s_last_streamed_row >= 0) {
            /* Close previous discontinuous burst */
            if (s_spi_dma_chan >= 0) {
                while (dma_channel_is_busy(s_spi_dma_chan)) {
                    if (s_background_task) s_background_task();
                    tight_loop_contents();
                }
            }
            while (spi_is_busy(ILI9341_SPI)) {
                if (s_background_task) s_background_task();
                tight_loop_contents();
            }
            cs_high();
        }

        cs_low();

        /* 1. Set row address (PASET) */
        dc_cmd();
        uint8_t cmd_paset = CMD_PASET;
        spi_write_blocking(ILI9341_SPI, &cmd_paset, 1);

        dc_data();
        uint8_t raset[4] = {
            (uint8_t)(row >> 8), (uint8_t)(row & 0xFF),
            (uint8_t)((ILI9341_HEIGHT - 1) >> 8), (uint8_t)((ILI9341_HEIGHT - 1) & 0xFF)
        };
        spi_write_blocking(ILI9341_SPI, raset, 4);

        /* 2. Start RAM write (RAMWR) */
        dc_cmd();
        uint8_t cmd_ramwr = CMD_RAMWR;
        spi_write_blocking(ILI9341_SPI, &cmd_ramwr, 1);
        dc_data();
    }

    s_last_streamed_row = (int16_t)row;

    /* 3. Send scanline pixel data via hardware DMA (or fallback) */
    if (s_spi_dma_chan >= 0) {
        dma_channel_configure(
            s_spi_dma_chan,
            &s_spi_dma_cfg,
            &spi_get_hw(ILI9341_SPI)->dr, /* write address */
            packed,                        /* read address */
            ILI9341_WIDTH * 2u,            /* 640 bytes */
            true                           /* start immediately */
        );
        while (dma_channel_is_busy(s_spi_dma_chan)) {
            if (s_background_task) {
                s_background_task();
            }
            tight_loop_contents();
        }
        while (spi_is_busy(ILI9341_SPI)) {
            if (s_background_task) {
                s_background_task();
            }
            tight_loop_contents();
        }
        while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    } else {
        spi_write_blocking(ILI9341_SPI, packed, ILI9341_WIDTH * 2u);
        while (spi_is_busy(ILI9341_SPI)) {
            if (s_background_task) {
                s_background_task();
            }
            tight_loop_contents();
        }
    }

    if (s_md_interleave) {
        /* In interleave mode, rows are not contiguous (parity steps by 2), so finish row */
        cs_high();
        s_last_streamed_row = -1;
    }
}

/* Core 1 background display worker */
static void __not_in_flash_func(nes_blitter_core1)(void) {
    while (1) {
        if (s_background_task) {
            s_background_task();
        }

        if (!s_blitter_active) {
            tight_loop_contents();
            continue;
        }

        uint8_t r = s_read_slot;
        if (__atomic_load_n(&s_slot_state[r], __ATOMIC_ACQUIRE)) {
            ili9341_stream_row(s_slot_data[r], s_slot_row[r]);
            __atomic_store_n(&s_slot_state[r], 0u, __ATOMIC_RELEASE);
            s_read_slot = (uint8_t)((r + 1u) % NES_LCD_SLOTS);
        } else {
            tight_loop_contents();
        }
    }
}

/* Pushes a row into the Core 1 slot queue with dirty-row skip and 256->320 scaling */
static void __not_in_flash_func(nes_band_push)(const uint16_t *rgb555_256, uint8_t row) {
    if (row >= ILI9341_HEIGHT) return;

    /* Dirty-row skip: only repaint rows that changed */
    uint32_t h = 2166136261u;
    for (uint16_t i = 0; i < 256; i++) {
        h = (h ^ rgb555_256[i]) * 16777619u;
    }
    if (s_scanlines_enabled && (row & 1u)) {
        h ^= 0x5A5A5A5Au;
    }
    if (h == s_row_hash[row]) {
        return;
    }
    s_row_hash[row] = h;

    const uint8_t slot = s_write_slot;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    uint8_t *dst = s_slot_data[slot];
    const uint16_t *xmap = s_band_xmap;
    const bool dim = s_scanlines_enabled && (row & 1u);
    for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
        uint16_t map_idx = xmap[x];
        uint16_t c;
        if (map_idx == 0xFFFFu) {
            c = 0x0000; /* Black side border */
        } else {
            c = rgb555_to_rgb565(rgb555_256[map_idx]);
            if (dim) {
                c = (c >> 1) & 0x7BEF;
            }
        }
        dst[2 * x + 0] = (uint8_t)(c >> 8);
        dst[2 * x + 1] = (uint8_t)(c & 0xFF);
    }
    s_slot_row[slot] = row;

    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

void ili9341_stream_nes_line(int line, const uint16_t *rgb555_256) {
    if (!s_blitter_active || line < 0 || line >= ILI9341_HEIGHT) return;

    /* Fill any skipped rows before this line (overscan 0..3) with backdrop on active field */
    while (s_next_row < (uint8_t)line) {
        if ((s_next_row & 1u) == s_field_parity) {
            for (int i = 0; i < 256; i++) s_backdrop_row[i] = s_current_backdrop555;
            nes_band_push(s_backdrop_row, s_next_row);
        }
        s_next_row++;
    }

    /* Render active scanline if on current field */
    if ((line & 1u) == s_field_parity) {
        nes_band_push(rgb555_256, (uint8_t)line);
    }
    s_next_row = (uint8_t)(line + 1);
}

void ili9341_stream_nes_frame_end(void) {
    if (!s_blitter_active) return;

    /* Fill remaining rows at bottom of screen (236..239) with backdrop on active field */
    while (s_next_row < ILI9341_HEIGHT) {
        if ((s_next_row & 1u) == s_field_parity) {
            for (int i = 0; i < 256; i++) s_backdrop_row[i] = s_current_backdrop555;
            nes_band_push(s_backdrop_row, s_next_row);
        }
        s_next_row++;
    }
    s_next_row = 0u;

    /* Sample current backdrop color from InfoNES palette */
    s_current_backdrop555 = PalTable[0];

    /* Invert field parity for next frame (interlaced display) */
    s_field_parity ^= 1u;
}

/* =========================================================================
 * PC Engine Streaming Support
 * ========================================================================= */
static int s_pce_screen_w = 256;
static int s_pce_screen_h = 240;
static int s_pce_panel_y = 0;
static uint16_t s_pce_xmap[ILI9341_WIDTH];
static bool s_pce_xmap_ready = false;

static void pce_prepare_band_xmap(void) {
    if (s_pce_xmap_ready) return;
    int w = s_pce_screen_w;
    if (w <= 0) w = 256;

    if (dmg_is_gb_mode()) {
        if (s_scale_mode == SCALE_INTEGER && w <= 266) {
            int border = 45 + (266 - w) / 2;
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < border || x >= border + w) {
                    s_pce_xmap[x] = 0xFFFFu;
                } else {
                    s_pce_xmap[x] = (uint16_t)(x - border);
                }
            }
        } else {
            /* STRETCH and FIT: scale across 266px DMG window (x=45..310) */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 45 || x >= 311) {
                    s_pce_xmap[x] = 0xFFFFu;
                } else {
                    s_pce_xmap[x] = (uint16_t)(((uint32_t)(x - 45) * (uint32_t)w) / 266u);
                }
            }
        }
    } else {
        if (s_scale_mode == SCALE_INTEGER) {
            if (w <= ILI9341_WIDTH) {
                int border = (ILI9341_WIDTH - w) / 2;
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < border || x >= border + w) {
                        s_pce_xmap[x] = 0xFFFFu;
                    } else {
                        s_pce_xmap[x] = (uint16_t)(x - border);
                    }
                }
            } else {
                int crop = (w - ILI9341_WIDTH) / 2;
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    s_pce_xmap[x] = (uint16_t)(x + crop);
                }
            }
        } else if (s_scale_mode == SCALE_FIT) {
            if (w <= 256) {
                /* 4:3 on 320x240 is 293 pixels wide, centered (13px border) */
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    if (x < 13 || x >= 306) {
                        s_pce_xmap[x] = 0xFFFFu;
                    } else {
                        s_pce_xmap[x] = (uint16_t)(((uint32_t)(x - 13) * (uint32_t)w) / 293u);
                    }
                }
            } else {
                /* Full width stretch */
                for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                    s_pce_xmap[x] = (uint16_t)(((uint32_t)x * (uint32_t)w) / (uint32_t)ILI9341_WIDTH);
                }
            }
        } else {
            /* SCALE_STRETCH */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                s_pce_xmap[x] = (uint16_t)(((uint32_t)x * (uint32_t)w) / (uint32_t)ILI9341_WIDTH);
            }
        }
    }
    s_pce_xmap_ready = true;
}

static void pce_band_push(const uint8_t *indexed_pce, const uint16_t *palette_rgb565, uint8_t row);

void ili9341_pce_set_geometry(int width, int height) {
    if (width <= 0) width = 256;
    if (height <= 0) height = 240;
    if (s_pce_screen_w != width || s_pce_screen_h != height) {
        s_pce_screen_w = width;
        s_pce_screen_h = height;
        s_pce_panel_y = (height < ILI9341_HEIGHT) ? (ILI9341_HEIGHT - height) / 2 : 0;
        s_pce_xmap_ready = false;
        pce_prepare_band_xmap();
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
        /* Fill top and bottom black borders once on geometry change */
        for (int r = 0; r < s_pce_panel_y; r++) {
            pce_band_push(NULL, NULL, (uint8_t)r);
        }
        for (int r = s_pce_panel_y + height; r < ILI9341_HEIGHT; r++) {
            pce_band_push(NULL, NULL, (uint8_t)r);
        }
    }
}

static void __not_in_flash_func(pce_band_push)(const uint8_t *indexed_pce, const uint16_t *palette_rgb565, uint8_t row) {
    if (row >= ILI9341_HEIGHT) return;

    if (indexed_pce == NULL) {
        if (s_row_hash[row] == 0u) return;
        s_row_hash[row] = 0u;

        const uint8_t slot = s_write_slot;
        while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
            tight_loop_contents();
        }
        memset(s_slot_data[slot], 0, NES_ROW_BYTES);
        s_slot_row[slot] = row;
        __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
        s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
        return;
    }

    /* Fast 32-bit FNV hash over the raw indexed input line (typically 64 words for 256px) */
    const uint32_t *src32 = (const uint32_t *)indexed_pce;
    uint32_t h = 2166136261u;
    int chunks = s_pce_screen_w >> 2;
    for (int i = 0; i < chunks; i++) {
        h = (h ^ src32[i]) * 16777619u;
    }
    if (s_scanlines_enabled && (row & 1u)) {
        h ^= 0x5A5A5A5Au;
    }

    /* If row hasn't changed, return immediately without any scaling or memory writes */
    if (h == s_row_hash[row]) return;
    s_row_hash[row] = h;

    pce_prepare_band_xmap();
    const uint16_t *xmap = s_pce_xmap;

    const uint8_t slot = s_write_slot;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    uint8_t *dst = s_slot_data[slot];
    const bool dim = s_scanlines_enabled && (row & 1u);

    if (!dim) {
        for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t map_idx = xmap[x];
            uint16_t c = (map_idx == 0xFFFFu || map_idx >= 368u) ? 0x0000 : palette_rgb565[indexed_pce[map_idx]];
            dst[2 * x + 0] = (uint8_t)(c >> 8);
            dst[2 * x + 1] = (uint8_t)(c & 0xFF);
        }
    } else {
        for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t map_idx = xmap[x];
            uint16_t c = (map_idx == 0xFFFFu || map_idx >= 368u) ? 0x0000 : palette_rgb565[indexed_pce[map_idx]];
            c = (c >> 1) & 0x7BEF; /* 50% CRT scanline attenuation */
            dst[2 * x + 0] = (uint8_t)(c >> 8);
            dst[2 * x + 1] = (uint8_t)(c & 0xFF);
        }
    }

    s_slot_row[slot] = row;
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

void ili9341_stream_pce_line(int line, const uint8_t *indexed_pce, const uint16_t *palette_rgb565) {
    if (!s_blitter_active || line < 0 || line >= s_pce_screen_h) return;

    uint8_t row = (uint8_t)(s_pce_panel_y + line);
    if (row >= ILI9341_HEIGHT) return;

    /* Always interlaced: only push lines on active field (60 fps locked) */
    if ((row & 1u) != s_field_parity) {
        return;
    }

    pce_band_push(indexed_pce, palette_rgb565, row);
}

void ili9341_stream_pce_frame_end(void) {
    if (!s_blitter_active) return;

    /* Always toggle field parity every frame for interlaced 60 fps */
    s_field_parity ^= 1u;
}

/* =========================================================================
 * Mega Drive / Genesis Streaming Support (60 FPS Interpolated)
 * ========================================================================= */
static int s_md_screen_w = 320;
static int s_md_screen_h = 224;
static int s_md_panel_y = 8;
static int s_md_dst_x = 0;
static int s_md_dst_y = 8;
static int s_md_dst_w = 320;
static int s_md_dst_h = 224;
static uint16_t s_md_xmap[ILI9341_WIDTH];
static bool s_md_xmap_ready = false;
static bool s_shimmerless_enabled = true;
static dmg_scale_weight_t s_md_xweights[ILI9341_WIDTH];
static int s_last_gb_mode = -1;

static uint8_t s_stretch_row_for_line[2][240];
static bool s_stretch_tables_init = false;
void md_update_interleave_field(void);

static void md_prepare_band_xmap(void) {
    const int gb_mode = dmg_is_gb_mode() ? 1 : 0;
    if (s_md_xmap_ready && s_last_gb_mode == gb_mode) return;
    s_last_gb_mode = gb_mode;

    int src_w = s_md_screen_w;
    if (src_w <= 0) src_w = 320;
    int src_h = s_md_screen_h;
    if (src_h <= 0) src_h = 224;
    if (src_h > 240) src_h = 240;

    const int vx = gb_mode ? (int)DMG_GB_WINDOW_X : 0;
    const int vw = gb_mode ? (int)DMG_GB_WINDOW_W : ILI9341_WIDTH;
    const int vy = 0;
    const int vh = ILI9341_HEIGHT;

    int dx, dy, dw, dh;

    if (gb_mode) {
        if (s_scale_mode == SCALE_INTEGER) {
            if (src_w <= vw) {
                /* H32 (256): 1:1 native centered inside 266px window (x=50..305, 5px border) */
                dw = src_w;
                dh = src_h;
                dx = vx + (vw - dw) / 2;
                dy = vy + (vh - dh) / 2;
            } else {
                /* H40 (320): 1:1 native center crop 266x224 (no horizontal scaling blur) */
                dw = vw;
                dh = src_h;
                dx = vx;
                dy = vy + (vh - dh) / 2;
            }
        } else if (s_scale_mode == SCALE_STRETCH) {
            /* Stretch: fill 100% of visible 266x240 DMG window */
            dw = vw;
            dh = vh;
            dx = vx;
            dy = vy;
        } else { /* SCALE_FIT */
            /* Proportional aspect ratio with letterbox:
             * For H40 (320x224): fit_w = 266, fit_h = (266 * 224 + 160) / 320 = 186
             * dy = (240 - 186) / 2 = 27 (27px black letterbox top & bottom)
             * For H32 (256x224): fit_w = 266, fit_h = (266 * 224 + 128) / 256 = 233
             * dy = (240 - 233) / 2 = 3 */
            int fit_w = vw;
            int fit_h = (vw * src_h + (src_w / 2)) / src_w;
            if (fit_h <= vh) {
                dw = fit_w;
                dh = fit_h;
            } else {
                dh = vh;
                dw = (vh * src_w + (src_h / 2)) / src_h;
            }
            dx = vx + (vw - dw) / 2;
            dy = vy + (vh - dh) / 2;
        }
    } else {
        /* Standard display (320x240) */
        if (src_w >= 320) {
            /* H40 Mode: 320 pixels */
            if (s_scale_mode == SCALE_FIT) {
                /* 4:3 Aspect Ratio (293 pixels centered with 13px borders) */
                dw = 293;
                dh = src_h;
                dx = 13;
                dy = (vh - dh) / 2;
            } else if (s_scale_mode == SCALE_STRETCH) {
                /* Full screen 320x240 stretched */
                dw = 320;
                dh = 240;
                dx = 0;
                dy = 0;
            } else {
                /* Integer 1:1 (320x224 centered at y=8) */
                dw = 320;
                dh = src_h;
                dx = 0;
                dy = (vh - dh) / 2;
            }
        } else {
            /* H32 mode: 256 pixels */
            if (s_scale_mode == SCALE_INTEGER) {
                /* Centered 256x224: 32px borders left/right */
                dw = 256;
                dh = src_h;
                dx = 32;
                dy = (vh - dh) / 2;
            } else if (s_scale_mode == SCALE_FIT) {
                /* 4:3 aspect ratio: 293px centered with 13px borders */
                dw = 293;
                dh = src_h;
                dx = 13;
                dy = (vh - dh) / 2;
            } else {
                /* Full screen stretch 320x240 */
                dw = 320;
                dh = 240;
                dx = 0;
                dy = 0;
            }
        }
    }

    s_md_dst_x = dx;
    s_md_dst_y = dy;
    s_md_dst_w = dw;
    s_md_dst_h = dh;
    s_md_panel_y = dy;

    static dmg_scale_weight_t temp[ILI9341_WIDTH];

    if (s_scale_mode == SCALE_INTEGER) {
        if (gb_mode && src_w > vw) {
            /* Center-crop 266px out of 320px */
            int start_x = (src_w - vw) / 2;
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < (uint16_t)dx || x >= (uint16_t)(dx + dw)) {
                    s_md_xmap[x] = 0xFFFFu;
                    s_md_xweights[x].src_idx = 0xFFFFu;
                    s_md_xweights[x].weight1 = 0;
                } else {
                    uint16_t sx = (uint16_t)(start_x + (int)x - dx);
                    s_md_xmap[x] = sx;
                    s_md_xweights[x].src_idx = sx;
                    s_md_xweights[x].weight1 = 0;
                }
            }
        } else {
            /* 1:1 centered */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < (uint16_t)dx || x >= (uint16_t)(dx + dw)) {
                    s_md_xmap[x] = 0xFFFFu;
                    s_md_xweights[x].src_idx = 0xFFFFu;
                    s_md_xweights[x].weight1 = 0;
                } else {
                    uint16_t sx = (uint16_t)((int)x - dx);
                    s_md_xmap[x] = sx;
                    s_md_xweights[x].src_idx = sx;
                    s_md_xweights[x].weight1 = 0;
                }
            }
        }
    } else if (dw == src_w) {
        /* 1:1 width */
        for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
            if (x < (uint16_t)dx || x >= (uint16_t)(dx + dw)) {
                s_md_xmap[x] = 0xFFFFu;
                s_md_xweights[x].src_idx = 0xFFFFu;
                s_md_xweights[x].weight1 = 0;
            } else {
                uint16_t sx = (uint16_t)((int)x - dx);
                s_md_xmap[x] = sx;
                s_md_xweights[x].src_idx = sx;
                s_md_xweights[x].weight1 = 0;
            }
        }
    } else {
        /* Scaled: init weights for src_w -> dw */
        dmg_init_shimmerless_weights((uint16_t)src_w, (uint16_t)dw, temp);
        for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
            if (x < (uint16_t)dx || x >= (uint16_t)(dx + dw)) {
                s_md_xmap[x] = 0xFFFFu;
                s_md_xweights[x].src_idx = 0xFFFFu;
                s_md_xweights[x].weight1 = 0;
            } else {
                uint16_t lx = (uint16_t)((int)x - dx);
                s_md_xmap[x] = (uint16_t)(((uint32_t)lx * (uint32_t)src_w) / (uint32_t)dw);
                s_md_xweights[x].src_idx = temp[lx].src_idx;
                s_md_xweights[x].weight1 = temp[lx].weight1;
            }
        }
    }

    /* Build vertical stretch/fit lookup table */
    memset(s_stretch_row_for_line, 0xFF, sizeof(s_stretch_row_for_line));
    for (int r = dy; r < dy + dh && r < ILI9341_HEIGHT; r++) {
        int s = ((r - dy) * src_h) / dh;
        if (s < 240) {
            int parity = r & 1;
            s_stretch_row_for_line[parity][s] = (uint8_t)r;
        }
    }

    s_stretch_tables_init = true;
    s_md_xmap_ready = true;
}

static void init_stretch_tables(void) {
    if (s_stretch_tables_init) return;
    md_prepare_band_xmap();
}

static void md_band_push(const uint16_t *rgb565_line, uint8_t row);

void ili9341_md_set_geometry(int width, int height) {
    if (width <= 0) width = 320;
    if (height <= 0) height = 224;
    if (s_md_screen_w != width || s_md_screen_h != height) {
        s_md_screen_w = width;
        s_md_screen_h = height;
        s_md_xmap_ready = false;
        md_prepare_band_xmap();
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
        /* Top and bottom borders when not full height */
        if (s_blitter_active && s_md_dst_y > 0) {
            for (int r = 0; r < s_md_dst_y && r < ILI9341_HEIGHT; r++) {
                s_row_hash[r] = 0u;
                md_band_push(NULL, (uint8_t)r);
            }
            for (int r = s_md_dst_y + s_md_dst_h; r < ILI9341_HEIGHT; r++) {
                s_row_hash[r] = 0u;
                md_band_push(NULL, (uint8_t)r);
            }
        }
        md_update_interleave_field();
    }
}

static inline char get_font_slice(char c, int row) {
    if (c < 32 || c >= (32 + 95)) return 0;
    return font_8x8[(c - 32) + row * 95];
}

static void __not_in_flash_func(md_band_push)(const uint16_t *rgb565_line, uint8_t row) {
    if (!s_blitter_active || row >= ILI9341_HEIGHT) return;

    if (rgb565_line == NULL) {
        if (s_row_hash[row] == 0u) return;
        s_row_hash[row] = 0u;

        const uint8_t slot = s_write_slot;
        uint32_t wait_t0 = time_us_32();
        while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
#if GWSND_OFFLOAD
            gwsnd_bridge_drain();
#endif
            if ((time_us_32() - wait_t0) > 2500u) {
                return;
            }
            tight_loop_contents();
        }
        memset(s_slot_data[slot], 0, NES_ROW_BYTES);
        s_slot_row[slot] = row;
        __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
        s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
        return;
    }

    /* Fast 32-bit FNV hash over the raw RGB565 input line (160 uint32 chunks for 320px) */
    const uint32_t *src32 = (const uint32_t *)rgb565_line;
    uint32_t h = 2166136261u;
    int chunks = s_md_screen_w >> 1;
    for (int i = 0; i < chunks; i++) {
        h = (h ^ src32[i]) * 16777619u;
    }
    if (s_scanlines_enabled && (row & 1u)) {
        h ^= 0x5A5A5A5Au;
    }
    /* Low-battery overlay: resend the row when it appears, changes or hides */
    dmg_batt_overlay_mark_row(row);
    s_bov_frame_rows = true;
    h ^= dmg_batt_overlay_row_salt(row);

    /* Anchor FPS overlay dynamically based on image vertical start row s_md_dst_y */
    const int fps_start_y = (s_md_dst_y > 8) ? (s_md_dst_y + 2) : 8;

    /* If row hasn't changed, return immediately without any scaling or memory writes */
    bool hud_active = dmg_volume_hud_is_active();
    if (h == s_row_hash[row] && (!s_fps_enabled || row < fps_start_y || row >= (fps_start_y + 8)) && (!hud_active || row < 180 || row >= 200)) return;

    md_prepare_band_xmap();
    const uint16_t *xmap = s_md_xmap;

    const uint8_t slot = s_write_slot;
    uint32_t wait_t1 = time_us_32();
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
#if GWSND_OFFLOAD
        gwsnd_bridge_drain();
#endif
        if ((time_us_32() - wait_t1) > 2500u) {
            return;
        }
        tight_loop_contents();
    }

    uint8_t *dst = s_slot_data[slot];
    const bool dim = s_scanlines_enabled && (row & 1u);
    const int max_w = s_md_screen_w;

    if (!dmg_is_gb_mode() && s_scale_mode != SCALE_FIT && s_md_screen_w >= 320) {
        /* Fast path: 1:1 320 width (Integer & Stretch in H40) via 32-bit rev16 stores */
        uint32_t *dst32 = (uint32_t *)dst;
        const uint32_t *src32_in = (const uint32_t *)rgb565_line;
        if (!dim) {
            for (int i = 0; i < ILI9341_WIDTH / 2; i++) {
                uint32_t p = src32_in[i];
                __asm("rev16 %0, %1" : "=r"(p) : "r"(p));
                dst32[i] = p;
            }
        } else {
            for (int i = 0; i < ILI9341_WIDTH / 2; i++) {
                uint32_t p = (src32_in[i] >> 1) & 0x7BEF7BEFu;
                __asm("rev16 %0, %1" : "=r"(p) : "r"(p));
                dst32[i] = p;
            }
        }
    } else {
        /* Scaled path with borders (Fit mode in H40, or H32 modes, or Stretch in DMG mode) */
        uint16_t *dst16 = (uint16_t *)dst;
        const bool do_blend = s_shimmerless_enabled && (s_scale_mode == SCALE_FIT);
        if (!do_blend) {
            /* Fast unrolled path: 0 bilinear overhead in Stretch and Integer modes */
            if (!dim) {
                for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                    uint16_t map_idx = xmap[x];
                    uint16_t c = (map_idx < max_w) ? rgb565_line[map_idx] : 0x0000;
                    dst16[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            } else {
                for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                    uint16_t map_idx = xmap[x];
                    uint16_t c = (map_idx < max_w) ? ((rgb565_line[map_idx] >> 1) & 0x7BEF) : 0x0000;
                    dst16[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            }
        } else {
            /* Shimmerless bilinear blend only in SCALE_FIT */
            if (!dim) {
                for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                    uint16_t map_idx = s_md_xweights[x].src_idx;
                    uint16_t c;
                    if (map_idx == 0xFFFFu || map_idx >= max_w) {
                        c = 0x0000;
                    } else {
                        uint16_t c0 = rgb565_line[map_idx];
                        if (s_md_xweights[x].weight1 > 0u && (map_idx + 1u < max_w)) {
                            uint16_t c1 = rgb565_line[map_idx + 1u];
                            c = dmg_blend_rgb565(c0, c1, s_md_xweights[x].weight1);
                        } else {
                            c = c0;
                        }
                    }
                    dst16[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            } else {
                for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                    uint16_t map_idx = s_md_xweights[x].src_idx;
                    uint16_t c;
                    if (map_idx == 0xFFFFu || map_idx >= max_w) {
                        c = 0x0000;
                    } else {
                        uint16_t c0 = rgb565_line[map_idx];
                        if (s_md_xweights[x].weight1 > 0u && (map_idx + 1u < max_w)) {
                            uint16_t c1 = rgb565_line[map_idx + 1u];
                            c = dmg_blend_rgb565(c0, c1, s_md_xweights[x].weight1);
                        } else {
                            c = c0;
                        }
                        c = (c >> 1) & 0x7BEF;
                    }
                    dst16[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            }
        }
    }

    /* Direct FPS overlay on lines fps_start_y .. fps_start_y + 7 */
    if (s_fps_enabled && row >= fps_start_y && row < (fps_start_y + 8)) {
        int r = (int)row - fps_start_y;
        int x_pos = dmg_is_gb_mode() ? (45 + 8) : 8;
        int max_x = dmg_is_gb_mode() ? 311 : ILI9341_WIDTH;
        for (int i = 0; s_fps_text[i] != '\0' && (x_pos + 8) < max_x; i++) {
            char slice = get_font_slice(s_fps_text[i], r);
            for (int bit = 0; bit < 8; bit++) {
                uint16_t c = (slice & (1 << bit)) ? 0x07E0 : 0x0000;
                dst[2 * (x_pos + bit) + 0] = (uint8_t)(c >> 8);
                dst[2 * (x_pos + bit) + 1] = (uint8_t)(c & 0xFF);
            }
            x_pos += 8;
        }
    }

    /* Volume OSD HUD overlay */
    int hud_x, hud_w;
    const uint16_t *hud = dmg_volume_hud_get_row(row, &hud_x, &hud_w);
    if (hud) {
        for (int i = 0; i < hud_w; i++) {
            uint16_t c = hud[i];
            if (c != 0x0001) {
                dst[2 * (hud_x + i) + 0] = (uint8_t)(c >> 8);
                dst[2 * (hud_x + i) + 1] = (uint8_t)(c & 0xFF);
            }
        }
    }

    dmg_batt_overlay_stamp_row(row, (uint16_t *)dst, 0, ILI9341_WIDTH, true);

    s_row_hash[row] = h;
    s_slot_row[slot] = row;
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

uint8_t md_line_active[240] = {0};
uint8_t md_line_dst_row[240] = {0};

void __not_in_flash_func(md_update_interleave_field)(void) {
    uint8_t parity = s_field_parity;
    if (!s_md_interleave) {
        init_stretch_tables();
        for (int i = 0; i < 240; i++) {
            md_line_active[i] = 1;
            if (s_scale_mode == SCALE_INTEGER || s_scale_mode == SCALE_FIT) {
                int r = s_md_dst_y + i;
                md_line_dst_row[i] = (r < ILI9341_HEIGHT) ? (uint8_t)r : 0xFF;
            } else {
                int r = (s_md_screen_h > 0) ? ((i * ILI9341_HEIGHT) / s_md_screen_h) : i;
                md_line_dst_row[i] = (r < ILI9341_HEIGHT) ? (uint8_t)r : 0xFF;
            }
        }
        return;
    }

    if (s_scale_mode == SCALE_INTEGER) {
        for (int i = 0; i < 240; i++) {
            int row = s_md_dst_y + i;
            if (row >= 0 && row < ILI9341_HEIGHT && ((row & 1u) == parity)) {
                md_line_active[i] = 1;
                md_line_dst_row[i] = (uint8_t)row;
            } else {
                md_line_active[i] = 0;
                md_line_dst_row[i] = 0xFF;
            }
        }
    } else {
        init_stretch_tables();
        for (int i = 0; i < 240; i++) {
            uint8_t row = s_stretch_row_for_line[parity][i];
            if (row != 0xFF && row < ILI9341_HEIGHT) {
                md_line_active[i] = 1;
                md_line_dst_row[i] = row;
            } else {
                md_line_active[i] = 0;
                md_line_dst_row[i] = 0xFF;
            }
        }
    }
}

void ili9341_set_interleave(bool enable) {
    if (s_md_interleave != enable) {
        s_md_interleave = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
        md_update_interleave_field();
    }
}

bool ili9341_get_interleave(void) {
    return s_md_interleave;
}

void ili9341_set_shimmerless(bool enable) {
    if (s_shimmerless_enabled != enable) {
        s_shimmerless_enabled = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_shimmerless(void) {
    return s_shimmerless_enabled;
}

bool __not_in_flash_func(ili9341_is_active_md_line)(int line, int height) {
    if (!s_blitter_active || line < 0 || line >= height || line >= 240) return false;
    return md_line_active[line] != 0;
}

void __not_in_flash_func(ili9341_stream_md_line_fast)(uint8_t row, const uint16_t *rgb565_line) {
    if (!s_blitter_active || row >= ILI9341_HEIGHT) return;
    md_band_push(rgb565_line, row);
}

void __not_in_flash_func(ili9341_stream_md_line)(int line, const uint16_t *rgb565_line, int width, int height) {
    if (!s_blitter_active || line < 0 || line >= height || line >= 240) return;

    if (s_md_screen_w != width || s_md_screen_h != height) {
        ili9341_md_set_geometry(width, height);
    }

    if (md_line_active[line]) {
        md_band_push(rgb565_line, md_line_dst_row[line]);
    }
}

/* Low-battery overlay rows the game did not emit this frame (above/below the
 * image): queued as black rows with the overlay stamped. Hash 0 = "black row",
 * so the NULL-line path does not repaint them blank. */
static void __not_in_flash_func(bov_queue_rows)(void) {
    if (!s_bov_frame_rows) return;
    s_bov_frame_rows = false;
    const uint32_t m = dmg_batt_overlay_frame_rows();
    if (m == 0u) return;
    const uint16_t y0 = dmg_batt_overlay_y0();
    for (uint16_t i = 0; i < dmg_batt_overlay_h(); i++) {
        if (!(m & (1u << i))) continue;
        const uint16_t row = (uint16_t)(y0 + i);
        const uint8_t slot = s_write_slot;
        const uint32_t t0 = time_us_32();
        bool ok = true;
        while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
#if GWSND_OFFLOAD
            gwsnd_bridge_drain();
#endif
            if ((time_us_32() - t0) > 2500u) { ok = false; break; }
            tight_loop_contents();
        }
        if (!ok) return;
        memset(s_slot_data[slot], 0, NES_ROW_BYTES);
        dmg_batt_overlay_stamp_row(row, (uint16_t *)s_slot_data[slot], 0, ILI9341_WIDTH, true);
        s_row_hash[row] = 0u;
        s_slot_row[slot] = (uint8_t)row;
        __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
        s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
    }
}

void __not_in_flash_func(ili9341_stream_md_frame_end)(void) {
    if (!s_blitter_active) return;
    bov_queue_rows();

    /* Toggle field parity every frame for interlaced 60 fps */
    if (s_md_interleave) {
        s_field_parity ^= 1u;
    }
    md_update_interleave_field();
}

void ili9341_blitter_start(void) {
    if (s_blitter_active) return;

    /* Launch Core 1 blitter once on the first invocation */
    if (!s_blitter_core1_launched) {
        s_blitter_core1_launched = true;
        multicore_launch_core1(nes_blitter_core1);
    }

    s_last_streamed_row = -1;
    nes_prepare_band_xmap();
    pce_prepare_band_xmap();
    md_prepare_band_xmap();
    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    for (int i = 0; i < ILI9341_HEIGHT; i++) {
        s_row_hash[i] = 0xFFFFFFFFu;
    }
    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        __atomic_store_n(&s_slot_state[i], 0u, __ATOMIC_RELEASE);
    }
    s_write_slot = 0u;
    s_read_slot = 0u;
    s_next_row = 0u;
    s_field_parity = 0u;
    s_current_backdrop555 = PalTable[0];

    s_blitter_active = true;
    md_update_interleave_field();
}

void ili9341_blitter_stop(void) {
    if (!s_blitter_active) return;

    s_last_streamed_row = -1;

    /* 1. Wait with safety timeout for Core 1 to drain active queue */
    uint32_t t0 = time_us_32();
    while ((time_us_32() - t0) < 50000u) {
        bool any_busy = false;
        for (int i = 0; i < NES_LCD_SLOTS; i++) {
            if (__atomic_load_n(&s_slot_state[i], __ATOMIC_ACQUIRE)) {
                any_busy = true;
                break;
            }
        }
        if (!any_busy) break;
        tight_loop_contents();
    }

    /* 2. Wait for SPI engine to complete */
    while (spi_is_busy(ILI9341_SPI)) {
        tight_loop_contents();
    }
    while (spi_is_readable(ILI9341_SPI)) {
        (void)spi_get_hw(ILI9341_SPI)->dr;
    }

    /* 3. Deselect ILI9341 */
    cs_high();

    /* 4. Put Core 1 into passive wait without resetting hardware */
    s_blitter_active = false;

    /* Clean all slots */
    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        __atomic_store_n(&s_slot_state[i], 0u, __ATOMIC_RELEASE);
    }
    s_write_slot = 0u;
    s_read_slot = 0u;
}

void ili9341_set_scanlines(bool enable) {
    if (s_scanlines_enabled != enable) {
        s_scanlines_enabled = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_scanlines(void) {
    return s_scanlines_enabled;
}

void ili9341_set_scale_mode(ili9341_scale_mode_t mode) {
    if (s_scale_mode != mode) {
        s_scale_mode = mode;
        s_band_xmap_ready = false;
        nes_prepare_band_xmap();
        s_pce_xmap_ready = false;
        pce_prepare_band_xmap();
        s_md_xmap_ready = false;
        md_prepare_band_xmap();
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
        /* When borders exist, explicitly clear top and bottom borders if blitter is active */
        if (s_blitter_active && s_md_dst_y > 0) {
            for (int r = 0; r < s_md_dst_y && r < ILI9341_HEIGHT; r++) {
                md_band_push(NULL, (uint8_t)r);
            }
            for (int r = s_md_dst_y + s_md_dst_h; r < ILI9341_HEIGHT; r++) {
                md_band_push(NULL, (uint8_t)r);
            }
        }
        md_update_interleave_field();
    }
}

ili9341_scale_mode_t ili9341_get_scale_mode(void) {
    return s_scale_mode;
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
    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        while (__atomic_load_n(&s_slot_state[i], __ATOMIC_ACQUIRE)) {
            tight_loop_contents();
        }
    }
}

bool ili9341_is_frame_busy(void) {
    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        if (__atomic_load_n(&s_slot_state[i], __ATOMIC_ACQUIRE)) return true;
    }
    return false;
}

/* Menu synchronous flush (flushes full 320x240 s_fb with RGB555->RGB565 conversion) */
void ili9341_flush(bool wait) {
    (void)wait;
    ili9341_wait_frame_done();

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    static uint16_t line_buf[ILI9341_WIDTH];
    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        const uint16_t *src = &s_fb[y * ILI9341_WIDTH];
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            line_buf[x] = rgb555_to_rgb565(src[x]);
        }
        spi_write16_blocking(ILI9341_SPI, line_buf, ILI9341_WIDTH);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void ili9341_flush_buffer(const uint16_t *buffer, bool wait) {
    (void)wait;
    ili9341_wait_frame_done();

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    static uint16_t line_buf[ILI9341_WIDTH];
    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        const uint16_t *src = &buffer[y * ILI9341_WIDTH];
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            line_buf[x] = rgb555_to_rgb565(src[x]);
        }
        spi_write16_blocking(ILI9341_SPI, line_buf, ILI9341_WIDTH);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
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
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    static uint16_t line_buf[ILI9341_WIDTH];
    for (int y = y_start; y <= y_end; y++) {
        const uint16_t *src = &s_fb[y * ILI9341_WIDTH];
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            line_buf[x] = rgb555_to_rgb565(src[x]);
        }
        spi_write16_blocking(ILI9341_SPI, line_buf, ILI9341_WIDTH);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    for (int y = y_start; y <= y_end; y++) {
        s_row_hash[y] = 0xFFFFFFFFu;
    }
}

/* Direct-to-panel block write, host-order RGB565, no framebuffer. Used by the
 * standby-resume intro on core 1 (shared_menu/dmg_boot_intro.c) while core 0
 * loads the game -- so it must not touch the framebuffer or the blitter state,
 * only the SPI. Blocking SPI on purpose: the DMA channel belongs to the blitter. */
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
