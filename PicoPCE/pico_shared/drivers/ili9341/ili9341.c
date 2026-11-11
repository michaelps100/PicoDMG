#include "ili9341.h"
#include "../../../pce_diag.h"
#include "dmg_menu.h"

static void st7789_init_sequence(void);
static void ili9341_init_sequence(void);

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

/* Dedicated DMA channel for Core 1 SPI scanline transmission */
static int s_spi_dma_chan = -1;
static dma_channel_config s_spi_dma_cfg;
static int16_t s_last_streamed_row = -1;

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
/* Shared display memory in SRAM (saves RAM, zero extra overhead)      */
/* Menu mode: full 320x240 RGB555 framebuffer (153.6 KB)              */
/* Gameplay:  2 x 240 x 320 Tear-Free Double Buffer (153.6 KB)        */
/* ------------------------------------------------------------------ */
static union {
    uint16_t fb[ILI9341_WIDTH * ILI9341_HEIGHT];
    uint8_t  pce_buf[2][ILI9341_HEIGHT * 320];
} __aligned(4) s_display_mem;

#define s_fb        (s_display_mem.fb)
#define s_pce_buf   (s_display_mem.pce_buf)

/* Double-buffered ping-pong line buffers for Core 1 DMA */
static uint8_t __aligned(4) s_dma_line_buf[2][ILI9341_WIDTH * 2u];

/* Frame synchronization between Core 0 and Core 1 */
static volatile uint8_t s_write_buf_idx = 0u;
static volatile uint8_t s_render_buf_idx = 0u;
static volatile uint8_t s_field_parity = 0u;
static volatile uint8_t s_render_field_parity = 0u;
static volatile bool s_field_ready = false;

static volatile bool s_blitter_core1_launched = false;
static volatile bool s_blitter_active = false;
static volatile bool s_scanlines_enabled = false;
static ili9341_scale_mode_t s_scale_mode = SCALE_FIT;

static bool s_pce_interleave = true; /* 0: Progressive (60fps), 1: Interleave (60fps) */
static uint16_t s_render_palette[256];
extern uint16_t paletteLUT[256];

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

/* MADCTL for the panel and rotation.
 *
 * The panel/mirror bits live in shared_menu (dmg_panel.h) so all nine emulators
 * agree. This used to hardcode 0x28 for every panel, which is right for the
 * PicoDMG's ILI9341 and wrong for the Core2350B's ST7789 -- it needs MX set
 * (0x60), and the symptom is a mirrored image that looks like a dozen other
 * faults. Anything calling set_rotation before the panel is up is fine: the
 * value is resolved again at power-on. */
static void apply_madctl(void) {
    write_cmd(CMD_MADCTL);
    write_data(dmg_panel_get_madctl());
}

void ili9341_set_rotation(uint8_t rotation) {
    /* The rotation index the project uses (1 = landscape, 3 = landscape
     * inverted) is kept for the callers that only care about the two landscape
     * orientations; the MADCTL itself now comes from shared_menu. */
    (void)rotation;
    apply_madctl();
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

    /* Switch SPI to operational baud rate */
    spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);
    ili9341_dma_init();

    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("PicoPCE display: %s MADCTL=0x%02X (%s) %s (%s) @ %u Hz (Core 1 DMA: %s)\n",
           dmg_panel_get() == DMG_PANEL_ST7789 ? "ST7789" : "ILI9341",
           (unsigned)dmg_panel_get_madctl(),
           dmg_panel_has_explicit_madctl() ? "from boot.txt" : "from panel table",
           dmg_panel_get_invert() ? "INVON" : "INVOFF",
           dmg_panel_source(),
           spi_get_baudrate(ILI9341_SPI),
           (s_spi_dma_chan >= 0) ? "ON" : "OFF");
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
}

/* ST7789 power-on: SWRESET -> SLPOUT -> INVON/INVOFF (per /boot.txt) ->
 * COLMOD 0x55 -> MADCTL -> address window -> NORON -> DISPON. Mirrors the
 * pico-bootLoader's runtime selection exactly, so the loader and the emulator
 * agree by construction rather than by having been debugged separately. */
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

    apply_madctl();


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

uint16_t *ili9341_get_framebuffer(void) {
    return s_fb;
}

uint16_t *ili9341_get_draw_buffer(void) {
    return s_fb;
}

/* =========================================================================
 * PC Engine Streaming Support
 * ========================================================================= */
static int s_pce_screen_w = 256;
static int s_pce_screen_h = 240;

/* Active window bounding box for hardware addressing (saves SPI bandwidth) */
static void get_active_window(uint16_t *win_x0, uint16_t *win_x1, uint16_t *win_w) {
    if (dmg_is_gb_mode()) {
        *win_x0 = 45u;
        *win_x1 = 310u;
        *win_w  = 266u;
    } else if (s_scale_mode == SCALE_INTEGER && s_pce_screen_w <= 256) {
        *win_x0 = 32u;
        *win_x1 = 287u;
        *win_w  = 256u;
    } else {
        *win_x0 = 0u;
        *win_x1 = (uint16_t)(ILI9341_WIDTH - 1u);
        *win_w  = (uint16_t)ILI9341_WIDTH;
    }
}

static int s_pce_win_x, s_pce_win_w, s_pce_win_h;
static int s_pce_disp_x, s_pce_disp_w, s_pce_disp_y, s_pce_disp_h;

static int16_t  s_pce_ymap[ILI9341_HEIGHT];
static int16_t  s_pce_row_first[ILI9341_HEIGHT];
static uint8_t  s_pce_row_count[ILI9341_HEIGHT];

static uint16_t s_pce_xmap[ILI9341_WIDTH];
static bool     s_pce_xmap_ready = false;
static bool     s_shimmerless_enabled = true;
static dmg_scale_weight_t s_pce_xweights[ILI9341_WIDTH];
static bool     s_pce_layout_ready = false;

static void pce_prepare_layout(void) {
    const int w = s_pce_screen_w > 0 ? s_pce_screen_w : 256;
    const int h = s_pce_screen_h > 0 ? s_pce_screen_h : 240;

    s_pce_win_x = dmg_is_gb_mode() ? 45 : 0;
    s_pce_win_w = dmg_is_gb_mode() ? 266 : ILI9341_WIDTH;
    s_pce_win_h = ILI9341_HEIGHT;

    int dw, dh;
    if (s_scale_mode == SCALE_STRETCH) {
        dw = s_pce_win_w;
        dh = s_pce_win_h;
    } else if (s_scale_mode == SCALE_FIT) {
        const int32_t sx = ((int32_t)s_pce_win_w << 16) / w;
        const int32_t sy = ((int32_t)s_pce_win_h << 16) / h;
        const int32_t s = (sx < sy) ? sx : sy;
        dw = (int)((((int32_t)w * s) + 0x8000) >> 16);
        dh = (int)((((int32_t)h * s) + 0x8000) >> 16);
    } else {
        dw = w;
        dh = h;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    if (dw > s_pce_win_w) dw = s_pce_win_w;
    if (dh > s_pce_win_h) dh = s_pce_win_h;

    s_pce_disp_w = dw;
    s_pce_disp_h = dh;
    s_pce_disp_x = s_pce_win_x + (s_pce_win_w - dw) / 2;
    s_pce_disp_y = (s_pce_win_h - dh) / 2;

    for (int i = 0; i < ILI9341_HEIGHT; i++) {
        s_pce_row_count[i] = 0;
    }
    int cur = -1;
    for (int r = 0; r < ILI9341_HEIGHT; r++) {
        if (r < s_pce_disp_y || r >= s_pce_disp_y + dh) {
            s_pce_ymap[r] = -1;
            continue;
        }
        int src = ((r - s_pce_disp_y) * h) / dh;
        if (src >= h) src = h - 1;
        s_pce_ymap[r] = (int16_t)src;
        if (src != cur) {
            cur = src;
            s_pce_row_first[src] = (int16_t)r;
        }
        s_pce_row_count[src]++;
    }

    s_pce_xmap_ready = false;
}

static void pce_prepare_band_xmap(void) {
    if (s_pce_xmap_ready) return;
    const int w  = s_pce_screen_w > 0 ? s_pce_screen_w : 256;
    const int dw = s_pce_disp_w > 0 ? s_pce_disp_w : 1;
    static dmg_scale_weight_t temp[ILI9341_WIDTH];

    if (w == dw || s_scale_mode == SCALE_INTEGER) {
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            if (x < s_pce_disp_x || x >= s_pce_disp_x + dw) {
                s_pce_xmap[x] = 0xFFFFu;
                s_pce_xweights[x].src_idx = 0xFFFFu;
                s_pce_xweights[x].weight1 = 0;
            } else {
                int src = x - s_pce_disp_x;
                if (src >= w) src = w - 1;
                s_pce_xmap[x] = (uint16_t)src;
                s_pce_xweights[x].src_idx = (uint16_t)src;
                s_pce_xweights[x].weight1 = 0;
            }
        }
    } else {
        dmg_init_shimmerless_weights((uint16_t)w, (uint16_t)dw, temp);
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            if (x < s_pce_disp_x || x >= s_pce_disp_x + dw) {
                s_pce_xmap[x] = 0xFFFFu;
                s_pce_xweights[x].src_idx = 0xFFFFu;
                s_pce_xweights[x].weight1 = 0;
            } else {
                int dx = x - s_pce_disp_x;
                int src = (dx * w) / dw;
                if (src >= w) src = w - 1;
                s_pce_xmap[x] = (uint16_t)src;
                s_pce_xweights[x].src_idx = temp[dx].src_idx;
                s_pce_xweights[x].weight1 = temp[dx].weight1;
            }
        }
    }
    s_pce_xmap_ready = true;
}

void ili9341_pce_set_geometry(int width, int height) {
    if (width <= 0) width = 256;
    if (height <= 0) height = 240;
    if (s_pce_screen_w != width || s_pce_screen_h != height || !s_pce_layout_ready) {
        s_pce_screen_w = width;
        s_pce_screen_h = height;
        s_pce_layout_ready = true;
        pce_prepare_layout();
        pce_prepare_band_xmap();
        if (s_blitter_active) {
            ili9341_fill_screen(0x0000);
        }
    }
}

static inline void pce_render_line_packed(
    uint8_t *line_packed,
    const uint8_t *src_indexed_320,
    uint16_t row,
    uint16_t win_x0,
    uint16_t win_w,
    bool dim,
    bool shimmerless
) {
    uint16_t *out16 = (uint16_t *)line_packed;
    const uint16_t max_w = (uint16_t)(s_pce_screen_w > 0 ? s_pce_screen_w : 256);

    if (src_indexed_320 == NULL) {
        memset(line_packed, 0, (size_t)win_w * 2u);
    } else if (shimmerless) {
        if (!dim) {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_pce_xweights[x].src_idx;
                uint16_t c;
                if (__builtin_expect(map_idx == 0xFFFFu || map_idx >= 368u, 0)) {
                    c = 0x0000;
                } else {
                    uint16_t c0 = s_render_palette[src_indexed_320[map_idx]];
                    uint8_t w1 = s_pce_xweights[x].weight1;
                    if (w1 > 0u && (map_idx + 1u < max_w)) {
                        uint16_t c1 = s_render_palette[src_indexed_320[map_idx + 1u]];
                        c = dmg_blend_rgb565(c0, c1, w1);
                    } else {
                        c = c0;
                    }
                }
                out16[i] = __builtin_bswap16(c);
            }
        } else {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_pce_xweights[x].src_idx;
                uint16_t c;
                if (__builtin_expect(map_idx == 0xFFFFu || map_idx >= 368u, 0)) {
                    c = 0x0000;
                } else {
                    uint16_t c0 = s_render_palette[src_indexed_320[map_idx]];
                    uint8_t w1 = s_pce_xweights[x].weight1;
                    if (w1 > 0u && (map_idx + 1u < max_w)) {
                        uint16_t c1 = s_render_palette[src_indexed_320[map_idx + 1u]];
                        c = dmg_blend_rgb565(c0, c1, w1);
                    } else {
                        c = c0;
                    }
                    c = (c >> 1) & 0x7BEFu;
                }
                out16[i] = __builtin_bswap16(c);
            }
        }
    } else {
        if (!dim) {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_pce_xweights[x].src_idx;
                uint16_t c = (__builtin_expect(map_idx == 0xFFFFu || map_idx >= 368u, 0)) ? 0x0000 : s_render_palette[src_indexed_320[map_idx]];
                out16[i] = __builtin_bswap16(c);
            }
        } else {
            for (uint16_t i = 0; i < win_w; i++) {
                uint16_t x = win_x0 + i;
                uint16_t map_idx = s_pce_xweights[x].src_idx;
                uint16_t c = (__builtin_expect(map_idx == 0xFFFFu || map_idx >= 368u, 0)) ? 0x0000 : s_render_palette[src_indexed_320[map_idx]];
                c = (c >> 1) & 0x7BEFu;
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

    /* Low-battery overlay: the part inside the active window */
    dmg_batt_overlay_stamp_row(row, out16, (int)win_x0, (int)win_w, true);
}

/* Sends a single scanline to the ILI9341 display with hardware DMA (Interleave mode) */
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

    /* 4. DMA transfer */
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

/* Core 1 background display worker (Ping-Pong DMA loop, fully decoupled from Core 0) */

/* Low-battery overlay: the part outside the game image (see
 * dmg_batt_overlay_push_borders). Pixels arrive native RGB565. */
static void __not_in_flash_func(bov_push_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                               const uint16_t *px) {
    set_addr_window(x, y, (uint16_t)(x + w - 1u), (uint16_t)(y + h - 1u));
    cs_low();
    dc_cmd();
    uint8_t cmd_ramwr = CMD_RAMWR;
    spi_write_blocking(ILI9341_SPI, &cmd_ramwr, 1);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    dc_data();
    uint16_t line[64];
    for (uint16_t r = 0; r < h; r++) {
        for (uint16_t i = 0; i < w && i < 64u; i++) line[i] = __builtin_bswap16(px[r * w + i]);
        spi_write_blocking(ILI9341_SPI, (const uint8_t *)line, (size_t)w * 2u);
    }
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    cs_high();
}

static void __not_in_flash_func(nes_blitter_core1)(void) {
    while (1) {
        if (!s_blitter_active) {
            tight_loop_contents();
            continue;
        }

        if (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
            pce_diag_stage(PCE_DIAG_CORE1_FRAME);
            const uint8_t *frame_data = s_pce_buf[s_render_buf_idx];
            const int16_t *ymap = s_pce_ymap;
            const bool scanlines = s_scanlines_enabled;
            const bool interleave = s_pce_interleave;
            const bool shimmerless = s_shimmerless_enabled && (s_scale_mode != SCALE_INTEGER);
            const uint8_t parity = s_render_field_parity;

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
                int16_t src_y0 = ymap[0];
                const uint8_t *src_line0 = (src_y0 >= 0) ? &frame_data[src_y0 * 320] : NULL;
                pce_render_line_packed(s_dma_line_buf[0], src_line0, 0, win_x0, win_w, false, shimmerless);

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
                    int16_t src_yi = ymap[i];
                    const uint8_t *src_line_i = (src_yi >= 0) ? &frame_data[src_yi * 320] : NULL;
                    bool dim_i = scanlines && (i & 1u);
                    pce_render_line_packed(cur_buf, src_line_i, i, win_x0, win_w, dim_i, shimmerless);

                    /* Wait for previous line DMA to finish */
                    if (s_spi_dma_chan >= 0) {
                        dma_channel_wait_for_finish_blocking(s_spi_dma_chan);
                    }

                    /* Launch DMA for current line */
                    if (s_spi_dma_chan >= 0) {
                        dma_channel_transfer_from_buffer_now(s_spi_dma_chan, cur_buf, line_bytes);
                    } else {
                        spi_write_blocking(ILI9341_SPI, cur_buf, line_bytes);
                    }
                }

                /* Wait for final line DMA to complete */
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
                    int16_t src_y = ymap[actual_row];
                    const uint8_t *src_line = (src_y >= 0) ? &frame_data[src_y * 320] : NULL;
                    const bool dim = scanlines && (actual_row & 1u);

                    pce_render_line_packed(s_dma_line_buf[0], src_line, actual_row, win_x0, win_w, dim, shimmerless);
                    ili9341_stream_row(s_dma_line_buf[0], actual_row, win_x0, win_x1, win_w);
                }
            }

            /* every row is sent, so only the columns beside the window are border */
            dmg_batt_overlay_push_borders((int)win_x0, (int)win_x0 + (int)win_w, 0, ILI9341_HEIGHT, bov_push_rect);

            __atomic_store_n(&s_field_ready, false, __ATOMIC_RELEASE);
            pce_diag_stage(PCE_DIAG_CORE1_DONE);
        } else {
            tight_loop_contents();
        }
    }
}

/* Core 0 per-scanline stream: copies indexed line directly into double buffer (non-blocking, ~0.25us) */
void ili9341_stream_pce_line(int line, const uint8_t *indexed_pce, const uint16_t *palette_rgb565) {
    (void)palette_rgb565;
    if (!s_blitter_active || line < 0 || line >= ILI9341_HEIGHT || indexed_pce == NULL) return;

    int w = s_pce_screen_w > 0 ? s_pce_screen_w : 256;
    if (w > 320) w = 320;
    memcpy(&s_pce_buf[s_write_buf_idx][line * 320], indexed_pce, (size_t)w);
}

/* Core 0 frame submission: flips double buffer and signals Core 1 */
void ili9341_stream_pce_frame_end(void) {
    if (!s_blitter_active) return;

    /* Snapshot current palette table for Core 1 */
    memcpy(s_render_palette, paletteLUT, sizeof(s_render_palette));

    /* Wait for Core 1 to finish previous frame if it's still busy */
    while (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    /* Submit current buffer to Core 1 */
    s_render_buf_idx = s_write_buf_idx;
    s_render_field_parity = s_field_parity;
    __atomic_store_n(&s_field_ready, true, __ATOMIC_RELEASE);

    /* Flip buffer index and parity for next frame */
    s_write_buf_idx ^= 1u;
    s_field_parity ^= 1u;
}

void ili9341_clear_black_frame(void) {
    ili9341_fill_screen(0x0000);
}

void ili9341_wait_frame_done(void) {
    if (!s_blitter_active) return;
    while (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }
}

void ili9341_blitter_start(void) {
    if (s_blitter_active) return;

    ili9341_dma_init();

    if (!s_blitter_core1_launched) {
        s_blitter_core1_launched = true;
        multicore_launch_core1(nes_blitter_core1);
    }

    pce_prepare_layout();
    pce_prepare_band_xmap();
    ili9341_fill_screen(0x0000);
    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    s_write_buf_idx = 0u;
    s_render_buf_idx = 0u;
    s_field_parity = 0u;
    s_render_field_parity = 0u;
    memset(s_pce_buf, 0, sizeof(s_pce_buf));
    memcpy(s_render_palette, paletteLUT, sizeof(s_render_palette));
    __atomic_store_n(&s_field_ready, false, __ATOMIC_RELEASE);
    s_blitter_active = true;
}

void ili9341_blitter_stop(void) {
    if (!s_blitter_active) return;
    s_blitter_active = false;

    /* Wait with safety timeout for Core 1 to complete current frame */
    uint32_t t0 = time_us_32();
    while (__atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE)) {
        if ((time_us_32() - t0) > 50000u) {
            __atomic_store_n(&s_field_ready, false, __ATOMIC_RELEASE);
            break;
        }
        tight_loop_contents();
    }

    if (s_spi_dma_chan >= 0) {
        dma_channel_wait_for_finish_blocking(s_spi_dma_chan);
    }
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    cs_high();
}

void ili9341_set_interleave(bool enable) {
    s_pce_interleave = enable;
}

bool ili9341_get_interleave(void) {
    return s_pce_interleave;
}

bool ili9341_is_active_pce_line(int line) {
    if (!s_pce_interleave) return true;
    if (line < 0 || line >= s_pce_screen_h) return false;
    const int n = s_pce_row_count[line];
    const int r0 = s_pce_row_first[line];
    for (int k = 0; k < n; k++) {
        if (((r0 + k) & 1u) == s_field_parity) return true;
    }
    return false;
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
        pce_prepare_layout();
        pce_prepare_band_xmap();
        if (s_blitter_active) {
            ili9341_fill_screen(0x0000);
        }
    }
}

ili9341_scale_mode_t ili9341_get_scale_mode(void) {
    return s_scale_mode;
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
    ili9341_stream_pce_frame_end();
}

bool ili9341_is_frame_busy(void) {
    return __atomic_load_n(&s_field_ready, __ATOMIC_ACQUIRE);
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
