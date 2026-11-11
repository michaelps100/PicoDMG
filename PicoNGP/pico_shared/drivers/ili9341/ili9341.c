#include "ili9341.h"
#include "dmg_menu.h"
#include "dmg_panel.h"
#include "dmg_scale.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/xip.h"
#include "hardware/sync.h"

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
#define SPI_RUN_BAUD   70000000u

/* Framebuffer used by the menu system (320x240 RGB555) */
static uint16_t __aligned(4) s_fb[ILI9341_WIDTH * ILI9341_HEIGHT];

/* ------------------------------------------------------------------ */
/* Per-scanline streaming blitter state (Dual-Core PicoDMG strategy)   */
/* ------------------------------------------------------------------ */
#define NES_LCD_SLOTS 48u
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

/* Active column window and payload sizing (Active Window Caging for 60 FPS Progressive) */
static volatile uint16_t s_active_x0 = 0;
static volatile uint16_t s_active_x1 = ILI9341_WIDTH - 1;
static volatile uint16_t s_active_width = ILI9341_WIDTH;
static volatile size_t s_active_row_bytes = NES_ROW_BYTES;
static volatile bool s_active_caset_dirty = true;
static uint8_t s_ngp_fit_xmap[253];
static bool s_ngp_fit_xmap_ready = false;

static uint8_t s_next_row = 0u;
static uint8_t s_field_parity = 0u; /* 0 or 1: interlaced fields to halve SPI bandwidth */
static uint32_t s_row_hash[ILI9341_HEIGHT];
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

/* Panel and orientation live in shared_menu (dmg_panel.h) so every emulator
 * driving the console agrees with the bootloader's choice, and so one firmware
 * serves either panel: ST7789= in /boot.txt decides at runtime. */
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
    /* Kept for API compatibility. The driver no longer has its own idea of the
     * orientation -- apply_madctl() is what the init sequences call. */
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

static void nes_prepare_band_xmap(void) {
    if (s_band_xmap_ready) return;
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
    s_band_xmap_ready = true;
}

/* ST7789 power-on, after the hardware reset. Deliberately short: it is the
 * sequence PicoGBC's mk_st7789.c runs in the field on this console, and the one
 * pico-bootLoader uses too, so the three agree by construction rather than by
 * having been debugged separately. The ILI9341-only power/gamma/DFUNCTR
 * registers below have no ST7789 equivalent and must not be sent to one. */
static void st7789_init_sequence(void) {
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(dmg_panel_get_invert() ? CMD_INVON : CMD_INVOFF);

    write_cmd(CMD_COLMOD);
    write_data(0x55); /* 16-bit RGB565 */

    apply_madctl();

    /* Physical panel timing. The factory ST7789 default sweeps GRAM at ~60 Hz
     * (16.6 ms), which is the same order as the time the Core 1 blitter takes to
     * push a frame over SPI, so the write and the scan collide in GRAM and show
     * up as a horizontal tear during full-screen transitions. Overclocking the
     * sweep to ~111 Hz (~9 ms) decouples the two.
     * Values live in shared_menu/dmg_panel.c -- one place, so a panel timing fix
     * is not applied to some emulators and forgotten in others. */
    dmg_panel_st7789_timing(panel_timing_write);

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    write_cmd(CMD_NORON);
    sleep_ms(10);

    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    write_cmd(CMD_DISPON);
    sleep_ms(120);
}

/* ILI9341 power-on: the recipe this driver has always run, unchanged apart from
 * MADCTL now coming from shared_menu. */
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

    apply_madctl();

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

void ili9341_init(void) {
    /* Panel and orientation come from /boot.txt via shared_menu, so this driver
     * no longer has its own idea of which display it is driving. Must run before
     * the sequences below, which read the resolved values. */
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

    nes_prepare_band_xmap();
    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("PicoNGP display: %s MADCTL=0x%02X (%s) %s (%s) @ %u Hz (Core 1 streaming)\n",
           dmg_panel_get() == DMG_PANEL_ST7789 ? "ST7789" : "ILI9341",
           (unsigned)dmg_panel_get_madctl(),
           dmg_panel_has_explicit_madctl() ? "from boot.txt" : "from panel table",
           dmg_panel_get_invert() ? "INVON" : "INVOFF",
           dmg_panel_source(),
           spi_get_baudrate(ILI9341_SPI));
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
        for (int r = 8; r < 16; r++) {
            s_row_hash[r] = 0u;
        }
    }
    if (fps_str && fps_str[0] != '\0') {
        if (strncmp(s_fps_text, fps_str, sizeof(s_fps_text) - 1) != 0) {
            strncpy(s_fps_text, fps_str, sizeof(s_fps_text) - 1);
            s_fps_text[sizeof(s_fps_text) - 1] = '\0';
            for (int r = 8; r < 16; r++) {
                s_row_hash[r] = 0u;
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
static uint16_t s_md_xmap[ILI9341_WIDTH];
static bool s_md_xmap_ready = false;

static uint8_t s_stretch_row_for_line[2][224];
static bool s_stretch_tables_init = false;

static void init_stretch_tables(void) {
    if (s_stretch_tables_init) return;
    memset(s_stretch_row_for_line, 0xFF, sizeof(s_stretch_row_for_line));
    for (int r = 0; r < ILI9341_HEIGHT; r++) {
        int s = (r * 224) / 240;
        int parity = r & 1;
        s_stretch_row_for_line[parity][s] = (uint8_t)r;
    }
    s_stretch_tables_init = true;
}

static void md_prepare_band_xmap(void) {
    if (s_md_xmap_ready) return;
    int w = s_md_screen_w;
    if (w <= 0) w = 320;

    if (w >= 320) {
        /* H40 Mode: 320 pixels */
        if (s_scale_mode == SCALE_FIT) {
            /* 4:3 Aspect Ratio: 293 pixels centered with 13px borders */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 13 || x >= 306) {
                    s_md_xmap[x] = 0xFFFFu;
                } else {
                    s_md_xmap[x] = (uint16_t)(((uint32_t)(x - 13) * 320u) / 293u);
                }
            }
        } else {
            /* SCALE_STRETCH & SCALE_INTEGER: full 320 width 1:1 */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                s_md_xmap[x] = x;
            }
        }
    } else {
        /* H32 mode: 256 pixels */
        if (s_scale_mode == SCALE_INTEGER) {
            /* Centered 256x224: 32px borders left/right */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 32 || x >= 288) {
                    s_md_xmap[x] = 0xFFFFu;
                } else {
                    s_md_xmap[x] = (uint16_t)(x - 32);
                }
            }
        } else if (s_scale_mode == SCALE_FIT) {
            /* 4:3 aspect ratio: 293px centered with ~13px borders */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 13 || x >= 306) {
                    s_md_xmap[x] = 0xFFFFu;
                } else {
                    s_md_xmap[x] = (uint16_t)(((uint32_t)(x - 13) * 256u) / 293u);
                }
            }
        } else {
            /* Full screen stretch */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                s_md_xmap[x] = (uint16_t)(((uint32_t)x * 256u) / (uint32_t)ILI9341_WIDTH);
            }
        }
    }
    s_md_xmap_ready = true;
}

static void md_band_push(const uint16_t *rgb565_line, uint8_t row);

void ili9341_md_set_geometry(int width, int height) {
    if (width <= 0) width = 320;
    if (height <= 0) height = 224;
    init_stretch_tables();
    if (s_md_screen_w != width || s_md_screen_h != height) {
        s_md_screen_w = width;
        s_md_screen_h = height;
        s_md_panel_y = (height < ILI9341_HEIGHT) ? (ILI9341_HEIGHT - height) / 2 : 0;
        s_md_xmap_ready = false;
        md_prepare_band_xmap();
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
        /* Top and bottom borders when not stretched */
        if (s_scale_mode != SCALE_STRETCH && s_md_panel_y > 0) {
            for (int r = 0; r < s_md_panel_y; r++) {
                s_row_hash[r] = 0u;
            }
            for (int r = s_md_panel_y + height; r < ILI9341_HEIGHT; r++) {
                s_row_hash[r] = 0u;
            }
        }
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
        while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
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

    /* If row hasn't changed, return immediately without any scaling or memory writes */
    if (h == s_row_hash[row] && (!s_fps_enabled || row < 8 || row >= 16)) return;
    s_row_hash[row] = h;

    md_prepare_band_xmap();
    const uint16_t *xmap = s_md_xmap;

    const uint8_t slot = s_write_slot;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    uint8_t *dst = s_slot_data[slot];
    const bool dim = s_scanlines_enabled && (row & 1u);
    const int max_w = s_md_screen_w;

    if (s_scale_mode != SCALE_FIT && s_md_screen_w >= 320) {
        /* Fast path: 1:1 320 width (Integer & Stretch in H40) */
        if (!dim) {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t c = rgb565_line[x];
                dst[2 * x + 0] = (uint8_t)(c >> 8);
                dst[2 * x + 1] = (uint8_t)(c & 0xFF);
            }
        } else {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t c = (rgb565_line[x] >> 1) & 0x7BEF;
                dst[2 * x + 0] = (uint8_t)(c >> 8);
                dst[2 * x + 1] = (uint8_t)(c & 0xFF);
            }
        }
    } else {
        /* Scaled path with borders (Fit mode in H40, or H32 modes) */
        if (!dim) {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t map_idx = xmap[x];
                uint16_t c = (map_idx == 0xFFFFu || map_idx >= max_w) ? 0x0000 : rgb565_line[map_idx];
                dst[2 * x + 0] = (uint8_t)(c >> 8);
                dst[2 * x + 1] = (uint8_t)(c & 0xFF);
            }
        } else {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t map_idx = xmap[x];
                uint16_t c = (map_idx == 0xFFFFu || map_idx >= max_w) ? 0x0000 : rgb565_line[map_idx];
                if (c != 0) c = (c >> 1) & 0x7BEF;
                dst[2 * x + 0] = (uint8_t)(c >> 8);
                dst[2 * x + 1] = (uint8_t)(c & 0xFF);
            }
        }
    }

    /* Direct FPS overlay on lines 8..15 */
    if (s_fps_enabled && row >= 8 && row < 16) {
        int r = (int)row - 8;
        int x_pos = 8;
        for (int i = 0; s_fps_text[i] != '\0' && (x_pos + 8) < ILI9341_WIDTH; i++) {
            char slice = get_font_slice(s_fps_text[i], r);
            for (int bit = 0; bit < 8; bit++) {
                uint16_t c = (slice & (1 << bit)) ? 0x07E0 : 0x0000;
                dst[2 * (x_pos + bit) + 0] = (uint8_t)(c >> 8);
                dst[2 * (x_pos + bit) + 1] = (uint8_t)(c & 0xFF);
            }
            x_pos += 8;
        }
    }

    s_slot_row[slot] = row;
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

bool __not_in_flash_func(ili9341_is_active_md_line)(int line, int height) {
    if (!s_blitter_active || line < 0 || line >= height) return false;
    if (s_scale_mode == SCALE_STRETCH && height == 224) {
        init_stretch_tables();
        return (s_stretch_row_for_line[s_field_parity][line] != 0xFF);
    }
    uint8_t row = (uint8_t)(s_md_panel_y + line);
    if (row >= ILI9341_HEIGHT) return false;
    return ((row & 1u) == s_field_parity);
}

void __not_in_flash_func(ili9341_stream_md_line)(int line, const uint16_t *rgb565_line, int width, int height) {
    if (!s_blitter_active || line < 0 || line >= height) return;

    if (s_md_screen_w != width || s_md_screen_h != height) {
        ili9341_md_set_geometry(width, height);
    }

    uint8_t row;
    if (s_scale_mode == SCALE_STRETCH && height == 224) {
        init_stretch_tables();
        row = s_stretch_row_for_line[s_field_parity][line];
        if (row == 0xFF || row >= ILI9341_HEIGHT) return;
    } else {
        row = (uint8_t)(s_md_panel_y + line);
        if (row >= ILI9341_HEIGHT) return;
        if ((row & 1u) != s_field_parity) return;
    }

    md_band_push(rgb565_line, row);
}

void __not_in_flash_func(ili9341_stream_md_frame_end)(void) {
    if (!s_blitter_active) return;

    /* Toggle field parity every frame for interlaced 60 fps */
    s_field_parity ^= 1u;
}

/* =========================================================================
 * NeoGeo Pocket Streaming Support (60 FPS Interpolated + Grid Effect)
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * NGP cross-core transport, on the PicoGBC distribution model.
 *
 * Core 0 (NeoPop) now does exactly one thing per scanline: copy the 160
 * native pixels into the ring below. Every per-pixel decision -- scale
 * expansion, grid dimming, volume HUD, FPS overlay, dirty-row hashing and
 * the SPI window -- belongs to Core 1.
 *
 * Before, Core 0 built all 240 display rows itself and blocked on a 48-slot
 * display-row ring, so the two cores were strictly serialised per row and the
 * frame cost was (Core 0 work) + (Core 1 work). Handing over the *native* line
 * instead makes the frame cost max(Core 0, Core 1), and the queue holds 32
 * native lines of look-ahead rather than 48 already-expanded display rows.
 *
 * s_slot_data is reused as the Core 1 staging area: it is already an array of
 * 640-byte rows, which is exactly what ngp_render_slot_pixels() writes, and
 * the NES/PCE/MD pushers that also reference it are never reached in this
 * project. That is why the transport costs no extra SRAM for the staging.
 * ------------------------------------------------------------------------- */
#define NGP_LINES         152u
#define NGP_LINE_PX       160u
#define NGP_LINE_BYTES    (NGP_LINE_PX * 2u)
#define NGP_RING_SLOTS    32u
#define NGP_TAG_FRAME_END 0xFFu

static uint8_t __aligned(4) s_ngp_native[NGP_RING_SLOTS][NGP_LINE_BYTES];
static volatile uint8_t s_ngp_native_state[NGP_RING_SLOTS];
static uint8_t s_ngp_native_tag[NGP_RING_SLOTS];
static uint8_t s_ngp_native_par[NGP_RING_SLOTS];
static volatile uint8_t s_ngp_native_write = 0u;
static volatile uint8_t s_ngp_native_read = 0u;

/* Core 1 private: the contiguous run of display rows being assembled. */
static uint16_t s_ngp_run_row0 = 0u;
static uint8_t s_ngp_run_count = 0u;
static uint16_t s_ngp_last_x0 = 0xFFFFu;
static uint16_t s_ngp_last_x1 = 0u;

/* Packed run staging. s_slot_data is 48 x 640 -- the exact worst case -- so the
 * run buffer is free, and a stride of s_active_row_bytes keeps each run one
 * contiguous range that leaves in a single spi_write_blocking(). */
/* The buffer is split in two 24-row halves that ping-pong: Core 1 expands the
 * next run into one half while the SPI DMA is still draining the other, so the
 * pixel work overlaps the bus instead of queueing behind it. */
#define NGP_STAGE_HALF_ROWS 24u
#define NGP_STAGE_HALF_BYTES (NGP_STAGE_HALF_ROWS * NES_ROW_BYTES)
static uint8_t s_ngp_half = 0u;           /* half currently being filled */
static bool s_ngp_dma_pending = false;    /* a run is still on the wire, CS low */
#define s_ngp_stage (((uint8_t *)s_slot_data) + (size_t)s_ngp_half * NGP_STAGE_HALF_BYTES)

/* blitter_stop() has to be certain no half-built run survives into the menu,
 * where Core 0 clears the panel and a late flush would repaint game pixels over
 * it. Core 1 owns the run, so stop asks for a flush and waits for the ack. */
static volatile bool s_ngp_flush_request = false;
static volatile bool s_ngp_flush_done = true;

/* Switching between the whole-frame DMA path and the run-based Interleave path
 * has to happen with Core 1 quiesced. The DMA path leaves a hardware window open
 * and a transfer in flight; if Core 1 starts issuing CASET/PASET/RAMWR for a run
 * on top of that, the two interleave on the same SPI and the panel ends up with a
 * corrupted address/RAMWR state -- a white screen, with the row stream no longer
 * meaning the same thing on the two cores. Core 0 asks for the resync and waits
 * for the ack before flipping the flag. */
static volatile bool s_ngp_resync_request = false;
static volatile bool s_ngp_resync_done = true;

static bool s_ngp_lut_ready = false;
static volatile bool s_grid_effect = false;

static uint16_t s_lut_ngp_to_rgb565[4096];
static void init_ngp_lut(void) {
    if (s_ngp_lut_ready) return;
    for (int i = 0; i < 4096; i++) {
        uint32_t r = i & 0x0F;
        uint32_t g = (i >> 4) & 0x0F;
        uint32_t b = (i >> 8) & 0x0F;
        uint32_t r5 = (r << 1) | (r >> 3);
        uint32_t g6 = (g << 2) | (g >> 2);
        uint32_t b5 = (b << 1) | (b >> 3);
        s_lut_ngp_to_rgb565[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
    s_ngp_lut_ready = true;
}

static inline uint16_t ili9341_grid_darken(uint16_t c) {
    /* ~14% darker: masks the pixel cell borders (LCD grid effect) */
    uint32_t r = (uint32_t)(c >> 11);
    uint32_t g = (uint32_t)((c >> 5) & 0x3Fu);
    uint32_t b = (uint32_t)(c & 0x1Fu);
    r = (r * 220u) >> 8;
    g = (g * 220u) >> 8;
    b = (b * 220u) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint8_t s_ngp_stretch_rows[2][152][2];
static uint8_t s_ngp_stretch_row_count[2][152];
static uint8_t s_ngp_prog_rows[152][2];
static uint8_t s_ngp_prog_row_count[152];
static bool s_ngp_stretch_inited = false;
static volatile bool s_ngp_interleave = false;

static void init_ngp_stretch_tables(void) {
    if (s_ngp_stretch_inited) return;
    memset(s_ngp_stretch_row_count, 0, sizeof(s_ngp_stretch_row_count));
    memset(s_ngp_prog_row_count, 0, sizeof(s_ngp_prog_row_count));
    for (int r = 0; r < ILI9341_HEIGHT; r++) {
        int s = (r * 152) / 240;
        int parity = r & 1;
        uint8_t cnt = s_ngp_stretch_row_count[parity][s];
        if (cnt < 2) {
            s_ngp_stretch_rows[parity][s][cnt] = (uint8_t)r;
            s_ngp_stretch_row_count[parity][s] = cnt + 1;
        }
        uint8_t pcnt = s_ngp_prog_row_count[s];
        if (pcnt < 2) {
            s_ngp_prog_rows[s][pcnt] = (uint8_t)r;
            s_ngp_prog_row_count[s] = pcnt + 1;
        }
    }
    s_ngp_stretch_inited = true;
}

static uint16_t s_ngp_xmap[ILI9341_WIDTH];
static bool s_ngp_xmap_ready = false;

static uint8_t s_ngp_stretch_gb_xmap[266];
static bool s_ngp_stretch_gb_xmap_ready = false;

static bool s_shimmerless_enabled = true;
static dmg_scale_weight_t s_ngp_fit_weights[253];
static bool s_ngp_fit_weights_ready = false;
static dmg_scale_weight_t s_ngp_stretch_gb_weights[266];
static bool s_ngp_stretch_gb_weights_ready = false;

static void ngp_prepare_xmap(void) {
    if (!s_ngp_fit_xmap_ready) {
        for (uint16_t x = 0; x < 253; x++) {
            s_ngp_fit_xmap[x] = (uint8_t)(((uint32_t)x * 160u) / 253u);
        }
        s_ngp_fit_xmap_ready = true;
    }
    if (!s_ngp_fit_weights_ready) {
        dmg_init_shimmerless_weights(160, 253, s_ngp_fit_weights);
        s_ngp_fit_weights_ready = true;
    }

    if (dmg_is_gb_mode()) {
        if (!s_ngp_stretch_gb_xmap_ready) {
            for (uint16_t x = 0; x < 266; x++) {
                s_ngp_stretch_gb_xmap[x] = (uint8_t)(((uint32_t)x * 160u) / 266u);
            }
            s_ngp_stretch_gb_xmap_ready = true;
        }
        if (!s_ngp_stretch_gb_weights_ready) {
            dmg_init_shimmerless_weights(160, 266, s_ngp_stretch_gb_weights);
            s_ngp_stretch_gb_weights_ready = true;
        }

        if (s_scale_mode == SCALE_FIT) {
            /* 253 px centered inside 266 window (x=45..310) -> offset 45 + (266 - 253)/2 = 51 */
            s_active_x0 = 51;
            s_active_x1 = 303;
            s_active_width = 253;
            s_active_row_bytes = 253 * 2u;
        } else if (s_scale_mode == SCALE_INTEGER) {
            /* 160 px centered inside 266 window (x=45..310) -> offset 45 + (266 - 160)/2 = 98 */
            s_active_x0 = 98;
            s_active_x1 = 257;
            s_active_width = 160;
            s_active_row_bytes = 160 * 2u;
        } else {
            /* SCALE_STRETCH: 266 px full DMG window (x=45..310) */
            s_active_x0 = 45;
            s_active_x1 = 310;
            s_active_width = 266;
            s_active_row_bytes = 266 * 2u;
        }
    } else {
        if (s_scale_mode == SCALE_FIT) {
            s_active_x0 = 34;
            s_active_x1 = 286;
            s_active_width = 253;
            s_active_row_bytes = 253 * 2u;
        } else if (s_scale_mode == SCALE_INTEGER) {
            s_active_x0 = 80;
            s_active_x1 = 239;
            s_active_width = 160;
            s_active_row_bytes = 160 * 2u;
        } else {
            /* SCALE_STRETCH */
            s_active_x0 = 0;
            s_active_x1 = 319;
            s_active_width = 320;
            s_active_row_bytes = 320 * 2u;
        }
    }
    s_active_caset_dirty = true;
    s_ngp_xmap_ready = true;
}

/* Integer mode only ever emits display rows 44..195, so both overlays have to
 * live inside that band. The FPS strip sits on the first 8 rows of the image
 * and the 20-row volume HUD moves up so that all 20 of its rows stay visible
 * (180..199 would lose its last 4). Outside Integer the original screen
 * coordinates are used unchanged. This is why the overlay used to be invisible
 * in Integer: rows 8..15 were never part of that window. */
static inline int ngp_fps_row0(void) { return (s_scale_mode == SCALE_INTEGER) ? 44 : 8; }
static inline int ngp_hud_row0(void) { return (s_scale_mode == SCALE_INTEGER) ? 152 : 180; }
static inline int ngp_hud_screen_y(int row) { return row + 180 - ngp_hud_row0(); }

static void __noinline __not_in_flash_func(ngp_render_slot_pixels)(uint8_t *dst, const uint16_t *ngp_line_160, uint8_t row) {
    const bool grid_on = s_grid_effect;
    uint16_t *dst16 = (uint16_t *)dst;

    if (__builtin_expect(s_scale_mode == SCALE_FIT, 1)) {
        /* Proportional ~1.05:1 PAR: 253 px active width */
        const uint32_t *src32 = (const uint32_t *)ngp_line_160;
        uint32_t w0 = src32[0];
        bool solid = true;
        for (int k = 1; k < 80; k++) {
            if (src32[k] != w0) { solid = false; break; }
        }

        if (!grid_on) {
            if (solid) {
                uint16_t c = __builtin_bswap16(s_lut_ngp_to_rgb565[ngp_line_160[0] & 0x0FFF]);
                for (int x = 0; x < 253; x++) dst16[x] = c;
            } else if (s_shimmerless_enabled) {
                for (int x = 0; x < 253; x++) {
                    uint16_t src_x = s_ngp_fit_weights[x].src_idx;
                    uint16_t c0 = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    uint8_t w1 = s_ngp_fit_weights[x].weight1;
                    uint16_t c;
                    if (w1 > 0u && src_x + 1u < 160u) {
                        uint16_t c1 = s_lut_ngp_to_rgb565[ngp_line_160[src_x + 1u] & 0x0FFF];
                        c = dmg_blend_rgb565(c0, c1, w1);
                    } else {
                        c = c0;
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            } else {
                for (int x = 0; x < 253; x++) {
                    uint8_t src_x = s_ngp_fit_xmap[x];
                    uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    dst16[x] = __builtin_bswap16(c);
                }
            }
        } else {
            const bool row_dim = (row & 1u);
            if (solid) {
                uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[0] & 0x0FFF];
                uint16_t c0 = __builtin_bswap16(row_dim ? ili9341_grid_darken(c) : c);
                uint16_t c1 = __builtin_bswap16(ili9341_grid_darken(c));
                for (int x = 0; x < 253; x += 2) {
                    dst16[x] = c0;
                    if (x + 1 < 253) dst16[x + 1] = c1;
                }
            } else if (s_shimmerless_enabled) {
                for (int x = 0; x < 253; x++) {
                    uint16_t src_x = s_ngp_fit_weights[x].src_idx;
                    uint16_t c0 = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    uint8_t w1 = s_ngp_fit_weights[x].weight1;
                    uint16_t c;
                    if (w1 > 0u && src_x + 1u < 160u) {
                        uint16_t c1 = s_lut_ngp_to_rgb565[ngp_line_160[src_x + 1u] & 0x0FFF];
                        c = dmg_blend_rgb565(c0, c1, w1);
                    } else {
                        c = c0;
                    }
                    if (row_dim || (x & 1u)) {
                        c = ili9341_grid_darken(c);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            } else {
                for (int x = 0; x < 253; x++) {
                    uint8_t src_x = s_ngp_fit_xmap[x];
                    uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    if (row_dim || (x & 1u)) {
                        c = ili9341_grid_darken(c);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            }
        }
    } else if (s_scale_mode == SCALE_INTEGER) {
        /* 1:1 centered: 160 px active width */
        if (!grid_on) {
            for (int x = 0; x < 160; x++) {
                uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[x] & 0x0FFF];
                dst16[x] = __builtin_bswap16(c);
            }
        } else {
            const bool row_dim = (row & 1u);
            for (int x = 0; x < 160; x++) {
                uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[x] & 0x0FFF];
                if (row_dim || (x & 1u)) {
                    c = ili9341_grid_darken(c);
                }
                dst16[x] = __builtin_bswap16(c);
            }
        }
    } else if (dmg_is_gb_mode()) {
        /* GB_MODE stretch: 160 -> 266 px */
        if (!grid_on) {
            if (s_shimmerless_enabled) {
                for (int x = 0; x < 266; x++) {
                    uint16_t src_x = s_ngp_stretch_gb_weights[x].src_idx;
                    uint16_t c0 = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    uint8_t w1 = s_ngp_stretch_gb_weights[x].weight1;
                    uint16_t c;
                    if (w1 > 0u && src_x + 1u < 160u) {
                        uint16_t c1 = s_lut_ngp_to_rgb565[ngp_line_160[src_x + 1u] & 0x0FFF];
                        c = dmg_blend_rgb565(c0, c1, w1);
                    } else {
                        c = c0;
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            } else {
                for (int x = 0; x < 266; x++) {
                    uint8_t src_x = s_ngp_stretch_gb_xmap[x];
                    uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    dst16[x] = __builtin_bswap16(c);
                }
            }
        } else {
            const bool row_dim = (row & 1u);
            if (s_shimmerless_enabled) {
                for (int x = 0; x < 266; x++) {
                    uint16_t src_x = s_ngp_stretch_gb_weights[x].src_idx;
                    uint16_t c0 = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    uint8_t w1 = s_ngp_stretch_gb_weights[x].weight1;
                    uint16_t c;
                    if (w1 > 0u && src_x + 1u < 160u) {
                        uint16_t c1 = s_lut_ngp_to_rgb565[ngp_line_160[src_x + 1u] & 0x0FFF];
                        c = dmg_blend_rgb565(c0, c1, w1);
                    } else {
                        c = c0;
                    }
                    if (row_dim || (x & 1u)) {
                        c = ili9341_grid_darken(c);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            } else {
                for (int x = 0; x < 266; x++) {
                    uint8_t src_x = s_ngp_stretch_gb_xmap[x];
                    uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[src_x] & 0x0FFF];
                    if (row_dim || (x & 1u)) {
                        c = ili9341_grid_darken(c);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            }
        }
    } else {
        /* Full width stretch 160 -> 320 px (x = 0..319) */
        if (!grid_on) {
            for (int i = 0; i < 160; i++) {
                uint16_t c = __builtin_bswap16(s_lut_ngp_to_rgb565[ngp_line_160[i] & 0x0FFF]);
                dst16[2 * i] = c;
                dst16[2 * i + 1] = c;
            }
        } else {
            const bool row_dim = (row & 1u);
            for (int i = 0; i < 160; i++) {
                uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[i] & 0x0FFF];
                uint16_t c0 = (row_dim) ? ili9341_grid_darken(c) : c;
                uint16_t c1 = ili9341_grid_darken(c);
                dst16[2 * i] = __builtin_bswap16(c0);
                dst16[2 * i + 1] = __builtin_bswap16(c1);
            }
        }
    }

    /* Direct FPS overlay, on the first 8 rows of the rendered image */
    const int fps0 = ngp_fps_row0();
    if (s_fps_enabled && row >= fps0 && row < fps0 + 8) {
        int r = (int)row - fps0;
        int x_pos = 8;
        for (int i = 0; s_fps_text[i] != '\0' && (x_pos + 8) < (int)s_active_width; i++) {
            char slice = get_font_slice(s_fps_text[i], r);
            for (int bit = 0; bit < 8; bit++) {
                if (slice & (1 << bit)) {
                    dst16[x_pos + bit] = __builtin_bswap16(0x07E0);
                }
            }
            x_pos += 8;
        }
    }
}

/* -------------------------------------------------------------------------
 * On-screen profiler panel.
 *
 * Drawn at 2x: at 1x the 8x8 font is unreadable on this panel, which is the
 * whole point of putting it there. 15 columns at 2x is 240 px, which still fits
 * the 253 px Fit window, and 5 lines is 80 rows.
 *
 * The text is rebuilt a few times a second, not per frame -- six snprintf per
 * frame would cost more than the panel does. The pixel stamping happens on the
 * Core 1 row path, so the panel costs no Core 0 time at all.
 * ------------------------------------------------------------------------- */
#if NGP_PROFILE
#define NGP_PROF_LINES 7
#define NGP_PROF_CHARS 15
#define NGP_PROF_W     (NGP_PROF_CHARS * 8 * 2)
#define NGP_PROF_H     (NGP_PROF_LINES * 8 * 2)

static char s_prof_text[NGP_PROF_LINES][NGP_PROF_CHARS + 1];
static bool s_prof_on = false;
static uint32_t s_prof_stamp_us = 0;
static uint32_t s_prof_fps = 0;
static uint32_t s_prof_frame_us = 0;

void ili9341_prof_enable(bool on) { s_prof_on = on; }

void ili9341_prof_feed(uint32_t fps, uint32_t frame_us, uint32_t emu_us) {
    s_prof_fps = fps;
    s_prof_frame_us = frame_us;
    ngp_prof_emu_us = emu_us;
}

static void __not_in_flash_func(ngp_prof_build)(void) {
    const uint32_t blocks = ngp_prof_blocks;
    const uint32_t tmr = ngp_prof_timers;
    const uint32_t emu = ngp_prof_emu_us;
    const uint32_t frame = s_prof_frame_us;
    const uint32_t pct = frame ? ((emu * 100u) / frame) : 0u;
    /* "OTH" is the whole point of the panel: frame time minus the emulated CPU
     * phase. That remainder is audio, the native-line hand-off and whatever
     * backpressure the display queue is applying to Core 0. */
    const uint32_t oth = (frame > emu) ? (frame - emu) : 0u;
    const uint32_t opct = frame ? ((oth * 100u) / frame) : 0u;

    snprintf(s_prof_text[0], NGP_PROF_CHARS + 1, "FPS%4lu T%2lu.%01lums",
             (unsigned long)s_prof_fps,
             (unsigned long)(frame / 1000u), (unsigned long)((frame / 100u) % 10u));
    snprintf(s_prof_text[1], NGP_PROF_CHARS + 1, "EMU%2lu.%01lums %3lu%%",
             (unsigned long)(emu / 1000u), (unsigned long)((emu / 100u) % 10u),
             (unsigned long)pct);
    snprintf(s_prof_text[2], NGP_PROF_CHARS + 1, "OTH%2lu.%01lums %3lu%%",
             (unsigned long)(oth / 1000u), (unsigned long)((oth / 100u) % 10u),
             (unsigned long)opct);
    /* TRUN is the decisive reading. updateTimers()'s inline fast path is
     * `timer_hint >= TIMER_HINT_RATE || (ram[0x20] & 0x0F)`, so while TRUN is
     * non-zero that fast path can never be taken and updateTimers_slow() runs
     * on every single instruction. A non-zero TRUN here means the "99% fast
     * path" comment in interrupt.h is false for this game. */
    /* XIP cache hit rate and misses over the last panel interval (~250 ms).
     * Counters are shared by flash and PSRAM and by both cores. Every miss on
     * the cartridge ROM is a PSRAM transaction, so a low hit rate here is the
     * signature of the interpreter fetching from PSRAM instead of from cache. */
    {
        const uint32_t hit = *(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_HIT_OFFSET);
        const uint32_t acc = *(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_ACC_OFFSET);
        *(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_HIT_OFFSET) = 0u;
        *(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_ACC_OFFSET) = 0u;
        const uint32_t pct = acc ? (uint32_t)(((uint64_t)hit * 100u) / acc) : 0u;
        const uint32_t miss = (acc > hit) ? (acc - hit) : 0u;
        snprintf(s_prof_text[3], NGP_PROF_CHARS + 1, "XIP%3lu%% M%5luK",
                 (unsigned long)pct, (unsigned long)(miss / 1000u));
    }
    snprintf(s_prof_text[4], NGP_PROF_CHARS + 1, "INS%5luK BL%4lu",
             (unsigned long)(ngp_prof_ins / 1000u), (unsigned long)blocks);
#if NGP_PROFILE_TIMERS
    snprintf(s_prof_text[5], NGP_PROF_CHARS + 1, "TMR%6luK G%5lu",
             (unsigned long)(tmr / 1000u), (unsigned long)ngp_prof_gfx);
#else
    /* STL = time Core 0 spent blocked on a full display ring. It is counted
     * inside EMU, so it is what tells "slow interpreter" from "slow panel". */
    snprintf(s_prof_text[5], NGP_PROF_CHARS + 1, "G%5lu STL%4lu",
             (unsigned long)ngp_prof_gfx, (unsigned long)(ngp_prof_stall_us / 1000u));
    /* Per-frame cost of the rasteriser and of the Z80, scaled by the 152 lines
     * a frame holds so it does not matter where in the 1 s window we sample. */
    {
        const uint32_t ln = ngp_prof_gfx ? ngp_prof_gfx : 1u;
        const uint32_t gfx_us = (uint32_t)(((uint64_t)ngp_prof_gfx_us * 152u) / ln);
        const uint32_t z80_us = (uint32_t)(((uint64_t)ngp_prof_z80_us * 152u) / ln);
        snprintf(s_prof_text[6], NGP_PROF_CHARS + 1, "GFX%2lu.%01lu Z%2lu.%01lums",
                 (unsigned long)(gfx_us / 1000u), (unsigned long)((gfx_us / 100u) % 10u),
                 (unsigned long)(z80_us / 1000u), (unsigned long)((z80_us / 100u) % 10u));
    }
#endif
}

/* Stamps the panel slice that belongs to one display row. Cheap: only the rows
 * inside the panel touch anything. */
static void __not_in_flash_func(ngp_prof_stamp_row)(uint8_t row, uint8_t *dst, int active_w) {
    if (!s_prof_on) return;

    const int y0 = (s_scale_mode == SCALE_INTEGER) ? 44 : 0;
    const int y = (int)row - y0;
    if (y < 0 || y >= NGP_PROF_H) return;

    const int w = (active_w < NGP_PROF_W) ? active_w : NGP_PROF_W;
    uint16_t *d16 = (uint16_t *)dst;

    /* Panel background so the text stays readable over any scene. */
    for (int x = 0; x < w; x++) d16[x] = 0x0000u;

    const int line = y >> 4;          /* 16 px per line at 2x */
    const int sub  = (y & 15) >> 1;   /* 8 font rows, each doubled */
    const char *txt = s_prof_text[line];

    for (int c = 0; txt[c] != '\0' && (c * 16) < w; c++) {
        const char slice = get_font_slice(txt[c], sub);
        if (slice == 0) continue;
        const int x0 = c * 16;
        for (int b = 0; b < 8; b++) {
            if (!(slice & (1 << b))) continue;
            const int px = x0 + b * 2;
            if (px + 1 >= w) break;
            d16[px] = 0xFFFFu;
            d16[px + 1] = 0xFFFFu;
        }
    }
}
#endif /* NGP_PROFILE */

/* s_row_hash now belongs exclusively to Core 1, so Core 0 must not write it.
 * It raises this flag instead and Core 1 does the invalidation. */
static volatile bool s_ngp_hash_reset = false;

/* Fast-forward forces the display to whole frames. NeoPop drops every other
 * emulated frame (system_frameskip_key), so the screen would otherwise keep
 * showing the previous complete frame while the interleave field parity keeps
 * flipping -- which is what produced the visible comb of half-updated rows at
 * 2x. With this set, the frames that do get drawn carry all 240 rows and the
 * skipped ones carry nothing, exactly like the PicoGBC model: complete frames,
 * every other game frame. The persisted Screen setting is left untouched. */
static volatile bool s_ngp_fast_forward = false;

/* Core 1 private: lets the volume HUD rows repaint once after it expires. */
static bool s_ngp_hud_was_active = false;

static void __not_in_flash_func(ngp_invalidate_hashes)(void) {
    for (int i = 0; i < ILI9341_HEIGHT; i++) {
        s_row_hash[i] = 0xFFFFFFFFu;
    }
}

/* Opens one hardware window for the assembled run and streams it with CS held
 * low for the whole batch. Rows that the dirty hash rejected are simply not
 * part of the run, so the panel keeps what GRAM already held.
 *
 * The staging is packed with a stride of exactly s_active_row_bytes, so the
 * whole run is one contiguous byte range and leaves in a single
 * spi_write_blocking() instead of one call per row. s_slot_data is 48 x 640,
 * the exact worst case (48 rows of 320 px), so the run buffer costs no SRAM. */
/* Completes the run handed to the DMA by the previous ngp_flush_run(): waits for
 * the last byte to leave the SPI, then releases CS. Safe to call when idle. */
static void __not_in_flash_func(ngp_dma_finish)(void) {
    if (!s_ngp_dma_pending) return;
    while (dma_channel_is_busy(s_spi_dma_chan)) {
        if (s_background_task) s_background_task();
        tight_loop_contents();
    }
    while (spi_is_busy(ILI9341_SPI)) {
        if (s_background_task) s_background_task();
        tight_loop_contents();
    }
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    cs_high();
    s_ngp_dma_pending = false;
}

/* Starts the assembled run and returns without waiting for the pixels: the
 * transfer is finished by the next flush (or by ngp_flush_run_sync). */
static void __not_in_flash_func(ngp_flush_run)(void) {
    if (s_ngp_run_count == 0u) return;

    /* The command bytes below share the SPI with the previous run's DMA. */
    ngp_dma_finish();

    const uint16_t x0 = s_active_x0;
    const uint16_t x1 = s_active_x1;
    const size_t row_bytes = s_active_row_bytes;
    const uint16_t row0 = s_ngp_run_row0;
    const uint16_t row1 = (uint16_t)(row0 + s_ngp_run_count - 1u);
    const uint8_t count = s_ngp_run_count;

    cs_low();

    if (s_active_caset_dirty || x0 != s_ngp_last_x0 || x1 != s_ngp_last_x1) {
        dc_cmd();
        uint8_t cmd = CMD_CASET;
        spi_write_blocking(ILI9341_SPI, &cmd, 1);
        dc_data();
        uint8_t caset[4] = {
            (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
            (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
        };
        spi_write_blocking(ILI9341_SPI, caset, 4);
        s_ngp_last_x0 = x0;
        s_ngp_last_x1 = x1;
        s_active_caset_dirty = false;
    }

    dc_cmd();
    uint8_t cmd_paset = CMD_PASET;
    spi_write_blocking(ILI9341_SPI, &cmd_paset, 1);
    dc_data();
    uint8_t raset[4] = {
        (uint8_t)(row0 >> 8), (uint8_t)(row0 & 0xFF),
        (uint8_t)(row1 >> 8), (uint8_t)(row1 & 0xFF)
    };
    spi_write_blocking(ILI9341_SPI, raset, 4);

    dc_cmd();
    uint8_t cmd_ramwr = CMD_RAMWR;
    spi_write_blocking(ILI9341_SPI, &cmd_ramwr, 1);

    dc_data();
    if (s_spi_dma_chan >= 0) {
        dma_channel_configure(
            s_spi_dma_chan,
            &s_spi_dma_cfg,
            &spi_get_hw(ILI9341_SPI)->dr,
            s_ngp_stage,
            row_bytes * count,
            true
        );
        /* CS stays low; the next flush (or the sync variant) closes it. Fill
         * the other half while this one is on the wire. */
        s_ngp_dma_pending = true;
        s_ngp_half ^= 1u;
    } else {
        spi_write_blocking(ILI9341_SPI, s_ngp_stage, row_bytes * count);
        while (spi_is_busy(ILI9341_SPI)) {
            if (s_background_task) s_background_task();
            tight_loop_contents();
        }
        cs_high();
    }

    s_ngp_run_count = 0u;
}

/* For the places that need the panel quiescent afterwards (menu hand-off, mode
 * resync, idle): flush and wait for the wire to go quiet. */
static void __not_in_flash_func(ngp_flush_run_sync)(void) {
    ngp_flush_run();
    ngp_dma_finish();
}


/* -------------------------------------------------------------------------
 * Core 1 side of the NGP transport. Everything below runs on Core 1 only.
 * ------------------------------------------------------------------------- */

/* Stages one display row, extending the current run when the row continues it.
 * `allow_copy` is set only when the row staged just before this one came from
 * the same native line and is its display neighbour: those two are pixel
 * identical unless the grid effect or an overlay is in play, so a memcpy
 * replaces a full re-expansion. Returns true when a row was actually staged. */
static bool __not_in_flash_func(ngp_stage_row)(uint8_t row, const uint16_t *native,
                                                uint32_t h, bool allow_copy) {
    if (s_ngp_run_count >= NGP_STAGE_HALF_ROWS ||
        (s_ngp_run_count > 0u && row != (uint16_t)(s_ngp_run_row0 + s_ngp_run_count))) {
        ngp_flush_run();
    }

    const bool hud_active = dmg_volume_hud_is_active();
    const int fps0 = ngp_fps_row0();
    const int hud0 = ngp_hud_row0();
    const bool overlay = (s_fps_enabled && row >= fps0 && row < fps0 + 8) ||
                         (hud_active && row >= hud0 && row < hud0 + 20);

    uint32_t hr = (s_grid_effect && (row & 1u)) ? (h ^ 0x5A5A5A5Au) : h;
    /* low-battery overlay: resend the row when it appears, changes or hides */
    hr ^= dmg_batt_overlay_row_salt(row);
    const bool bov = dmg_batt_overlay_row_hit(row);
    if (!overlay && hr == s_row_hash[row]) {
        return false; /* GRAM still holds this row */
    }
    s_row_hash[row] = hr;

    if (s_ngp_run_count == 0u) {
        s_ngp_run_row0 = row;
    }

    const size_t stride = s_active_row_bytes;
    uint8_t *dst = s_ngp_stage + (size_t)s_ngp_run_count * stride;
    if (allow_copy && !s_grid_effect && !overlay && !bov && s_ngp_run_count > 0u) {
        memcpy(dst, dst - stride, stride);
    } else {
        ngp_render_slot_pixels(dst, native, row);

        /* Volume OSD HUD overlay */
        int hud_x, hud_w;
        const uint16_t *hud = dmg_volume_hud_get_row(ngp_hud_screen_y(row), &hud_x, &hud_w);
        if (hud) {
            uint16_t *dst16 = (uint16_t *)dst;
            const int start_x = s_active_x0;
            const int active_w = (int)s_active_width;
            for (int i = 0; i < hud_w; i++) {
                int local_x = (hud_x + i) - start_x;
                if (local_x >= 0 && local_x < active_w) {
                    uint16_t c = hud[i];
                    if (c != 0x0001) {
                        dst16[local_x] = __builtin_bswap16(c);
                    }
                }
            }
        }
        dmg_batt_overlay_stamp_row(row, (uint16_t *)dst, (int)s_active_x0, (int)s_active_width, true);
#if NGP_PROFILE
        ngp_prof_stamp_row(row, dst, (int)s_active_width);
#endif
    }
    s_ngp_run_count++;
    return true;
}

/* Expands one native scanline into the display rows it owns. `par` travels
 * with the line so Core 1 uses the exact field parity Core 0 rasterised for,
 * with no chance of the two drifting apart. Interleave path only. */
static void __not_in_flash_func(ngp_process_line)(uint8_t line, uint8_t par,
                                                  const uint16_t *native) {
    if (line >= NGP_LINES || native == NULL) return;

    const uint32_t *src32 = (const uint32_t *)native;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 80; i++) {
        h = (h ^ src32[i]) * 16777619u;
    }

    uint8_t rows[2];
    uint8_t cnt;

    if (s_scale_mode == SCALE_INTEGER) {
        const uint8_t row = (uint8_t)(44u + line);
        if (row >= ILI9341_HEIGHT) return;
        if (s_ngp_interleave && ((row & 1u) != par)) return;
        rows[0] = row;
        cnt = 1u;
    } else if (s_ngp_interleave) {
        cnt = s_ngp_stretch_row_count[par][line];
        for (uint8_t i = 0; i < cnt; i++) {
            rows[i] = s_ngp_stretch_rows[par][line][i];
        }
    } else {
        cnt = s_ngp_prog_row_count[line];
        for (uint8_t i = 0; i < cnt; i++) {
            rows[i] = s_ngp_prog_rows[line][i];
        }
    }
    if (cnt == 0u) return;

    /* `prev_staged` guards the memcpy fast path: it is only valid when the
     * row staged immediately before is the other display row of THIS native
     * line. Without it, a line whose first row was hash-clean would copy the
     * previous line's pixels. */
    bool prev_staged = false;
    for (uint8_t i = 0; i < cnt; i++) {
        const bool copy_src = (i == 1u) && prev_staged &&
                              (rows[1] == (uint8_t)(rows[0] + 1u));
        prev_staged = ngp_stage_row(rows[i], native, h, copy_src);
    }
}


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

/* Core 1 display loop. */
static void __not_in_flash_func(ngp_blitter_core1)(void) {
    for (;;) {
        if (s_background_task) {
            s_background_task();
        }

        if (s_ngp_flush_request) {
            s_ngp_flush_request = false;
            ngp_flush_run_sync();
            s_ngp_flush_done = true;
        }

        if (s_ngp_resync_request) {
            s_ngp_resync_request = false;
            ngp_flush_run_sync();
            s_ngp_run_count = 0u;
            s_ngp_run_row0 = 0u;
            s_ngp_last_x0 = 0xFFFFu;
            s_ngp_last_x1 = 0u;
            s_ngp_hud_was_active = false;
            s_active_caset_dirty = true;
            s_ngp_resync_done = true;
            continue;
        }

        if (!s_blitter_active) {
            ngp_flush_run_sync();
            if (s_background_task) s_background_task();
            tight_loop_contents();
            continue;
        }

        if (s_ngp_hash_reset) {
            s_ngp_hash_reset = false;
            ngp_invalidate_hashes();
        }

        const uint8_t slot = s_ngp_native_read;
        if (!__atomic_load_n(&s_ngp_native_state[slot], __ATOMIC_ACQUIRE)) {
            if (s_background_task) s_background_task();
            tight_loop_contents();
            continue;
        }

        const uint8_t tag = s_ngp_native_tag[slot];
        const uint8_t par = s_ngp_native_par[slot];

        if (tag == NGP_TAG_FRAME_END) {
            ngp_flush_run();
            /* Low-battery overlay outside the image: Integer only emits rows
             * 44..195, and every mode crops the width to the active window. */
            if (dmg_batt_overlay_borders_pending()) {
                ngp_dma_finish();
                const int gx0 = (int)s_active_x0, gx1 = (int)s_active_x0 + (int)s_active_width;
                const int gy0 = (s_scale_mode == SCALE_INTEGER) ? 44 : 0;
                const int gy1 = (s_scale_mode == SCALE_INTEGER) ? 196 : ILI9341_HEIGHT;
                dmg_batt_overlay_push_borders(gx0, gx1, gy0, gy1, bov_push_rect);
                s_active_caset_dirty = true;
            }
#if NGP_PROFILE
            if (s_prof_on) {
                const uint32_t now = time_us_32();
                if ((int32_t)(now - s_prof_stamp_us) >= 250000) {
                    s_prof_stamp_us = now;
                    ngp_prof_build();
                }
            }
#endif
            const int fps0 = ngp_fps_row0();
            const int hud0 = ngp_hud_row0();
            if (s_fps_enabled) {
                for (int r = fps0; r < fps0 + 8; r++) s_row_hash[r] = 0xFFFFFFFFu;
            }
            const bool hud_now = dmg_volume_hud_is_active();
            if (hud_now || s_ngp_hud_was_active) {
                for (int r = hud0; r < hud0 + 20; r++) s_row_hash[r] = 0xFFFFFFFFu;
            }
            s_ngp_hud_was_active = hud_now;
        } else {
            ngp_process_line(tag, par, (const uint16_t *)s_ngp_native[slot]);
        }

        __atomic_store_n(&s_ngp_native_state[slot], 0u, __ATOMIC_RELEASE);
        s_ngp_native_read = (uint8_t)((slot + 1u) % NGP_RING_SLOTS);
    }
}

bool __not_in_flash_func(ili9341_is_active_ngp_line)(int line) {
    if (!s_blitter_active || line < 0 || line >= (int)NGP_LINES) return false;
    if (!s_ngp_interleave || s_ngp_fast_forward) return true;
    if (s_scale_mode == SCALE_INTEGER) {
        uint8_t row = (uint8_t)(44 + line);
        if (row >= ILI9341_HEIGHT) return false;
        return ((row & 1u) == s_field_parity);
    } else {
        init_ngp_stretch_tables();
        return (s_ngp_stretch_row_count[s_field_parity][line] > 0);
    }
}

/* Core 0 side of the transport, and the only per-scanline work left here: one
 * 320-byte copy of the native line into the ring. No hashing, no scaling, no
 * window handling, and no waiting on Core 1 beyond a full ring. */
void __not_in_flash_func(ili9341_stream_ngp_line)(int line, const uint16_t *ngp_line_160) {
    if (!s_blitter_active || line < 0 || line >= (int)NGP_LINES || ngp_line_160 == NULL) return;

    const uint8_t slot = s_ngp_native_write;
#if NGP_PROFILE
    if (__atomic_load_n(&s_ngp_native_state[slot], __ATOMIC_ACQUIRE)) {
        const uint32_t t_stall = time_us_32();
        while (__atomic_load_n(&s_ngp_native_state[slot], __ATOMIC_ACQUIRE)) {
            tight_loop_contents();
        }
        ngp_prof_stall_us += time_us_32() - t_stall;
    }
#else
    while (__atomic_load_n(&s_ngp_native_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }
#endif

    memcpy(s_ngp_native[slot], ngp_line_160, NGP_LINE_BYTES);
    s_ngp_native_tag[slot] = (uint8_t)line;
    s_ngp_native_par[slot] = s_field_parity;

    __atomic_store_n(&s_ngp_native_state[slot], 1u, __ATOMIC_RELEASE);
    s_ngp_native_write = (uint8_t)((slot + 1u) % NGP_RING_SLOTS);
}

void __not_in_flash_func(ili9341_stream_ngp_frame_end)(void) {
    if (!s_blitter_active) return;

    /* Sentinel, not a pixel: it only tells Core 1 the frame is complete so it
     * closes the window/batch and refreshes the rows the pixel hash cannot see. */
    const uint8_t slot = s_ngp_native_write;
    while (__atomic_load_n(&s_ngp_native_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }
    s_ngp_native_tag[slot] = NGP_TAG_FRAME_END;
    s_ngp_native_par[slot] = s_field_parity;
    __atomic_store_n(&s_ngp_native_state[slot], 1u, __ATOMIC_RELEASE);
    s_ngp_native_write = (uint8_t)((slot + 1u) % NGP_RING_SLOTS);

    if (s_ngp_interleave) {
        s_field_parity ^= 1u;
    }
}

/* Core 0 side of the mode switch. Must run BEFORE the flag changes, so Core 1 is
 * still parked on the old path when it drains. */
static void __not_in_flash_func(ngp_resync_core1)(void) {
    if (!s_blitter_core1_launched) return;

    s_ngp_resync_done = false;
    s_ngp_resync_request = true;
    uint32_t t = time_us_32();
    while (!s_ngp_resync_done && (time_us_32() - t) < 60000u) {
        tight_loop_contents();
    }
}

void ili9341_set_interleave(bool enable) {
    if (s_ngp_interleave != enable) {
        ngp_resync_core1();
        s_ngp_interleave = enable;
        s_ngp_hash_reset = true;
    }
}

bool ili9341_get_interleave(void) {
    return s_ngp_interleave;
}

void ili9341_set_fast_forward(bool enable) {
    if (s_ngp_fast_forward != enable) {
        ngp_resync_core1();
        s_ngp_fast_forward = enable;
        /* The first whole frame has to repaint every row: the other field's
         * rows are still one frame stale from Interleave. */
        s_ngp_hash_reset = true;
    }
}

bool ili9341_get_fast_forward(void) {
    return s_ngp_fast_forward;
}

void ili9341_set_grid_effect(bool enable) {
    if (s_grid_effect != enable) {
        s_grid_effect = enable;
        s_scanlines_enabled = enable;
        s_ngp_hash_reset = true;
    }
}

bool ili9341_get_grid_effect(void) {
    return s_grid_effect;
}

void ili9341_blitter_start(void) {
    if (s_blitter_active) return;

    ili9341_dma_init();

    /* Launch Core 1 blitter once on the first invocation */
    if (!s_blitter_core1_launched) {
        s_blitter_core1_launched = true;
        multicore_launch_core1(ngp_blitter_core1);
    }

    nes_prepare_band_xmap();
    pce_prepare_band_xmap();
    md_prepare_band_xmap();
    /* Core 1 does all the pixel work, so the palette LUT, the 152->240 row maps
     * and the active window must exist before the first line is dispatched. */
    init_ngp_lut();
    init_ngp_stretch_tables();
    ngp_prepare_xmap();
    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        __atomic_store_n(&s_slot_state[i], 0u, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < NGP_RING_SLOTS; i++) {
        __atomic_store_n(&s_ngp_native_state[i], 0u, __ATOMIC_RELEASE);
        s_ngp_native_tag[i] = NGP_TAG_FRAME_END;
        s_ngp_native_par[i] = 0u;
    }
    s_ngp_native_write = 0u;
    s_ngp_native_read = 0u;
    s_ngp_run_row0 = 0u;
    s_ngp_run_count = 0u;
    s_ngp_half = 0u;
    s_ngp_dma_pending = false;
    s_ngp_last_x0 = 0xFFFFu;
    s_ngp_last_x1 = 0u;
    s_ngp_hud_was_active = false;
    s_ngp_flush_request = false;
    s_ngp_flush_done = true;
    s_ngp_resync_request = false;
    s_ngp_resync_done = true;
    s_ngp_hash_reset = true;
    s_write_slot = 0u;
    s_read_slot = 0u;
    s_next_row = 0u;
    s_field_parity = 0u;
    s_current_backdrop555 = PalTable[0];
    s_active_caset_dirty = true;

    s_blitter_active = true;
}

void ili9341_blitter_stop(void) {
    if (!s_blitter_active) return;

    /* 1. Wait with safety timeout for Core 1 to drain the native line ring.
     * A freed slot means the line was consumed. */
    uint32_t t0 = time_us_32();
    while ((time_us_32() - t0) < 50000u) {
        bool any_busy = false;
        for (int i = 0; i < NGP_RING_SLOTS; i++) {
            if (__atomic_load_n(&s_ngp_native_state[i], __ATOMIC_ACQUIRE)) {
                any_busy = true;
                break;
            }
        }
        if (!any_busy) break;
        tight_loop_contents();
    }

    /* 2. Make Core 1 close any half-built batch or window before Core 0 touches
     * the panel. Without this a late flush would repaint game pixels over the
     * black that the menu draws next. */
    s_ngp_flush_done = false;
    s_ngp_flush_request = true;
    {
        uint32_t t1 = time_us_32();
        while (!s_ngp_flush_done && (time_us_32() - t1) < 50000u) {
            tight_loop_contents();
        }
    }

    /* 3. Wait for SPI engine to complete */
    while (spi_is_busy(ILI9341_SPI)) {
        tight_loop_contents();
    }
    while (spi_is_readable(ILI9341_SPI)) {
        (void)spi_get_hw(ILI9341_SPI)->dr;
    }

    /* 4. Deselect ILI9341 */
    cs_high();

    /* 5. Put Core 1 into passive wait without resetting hardware */
    s_blitter_active = false;

    /* Clean all slots */
    for (int i = 0; i < NGP_RING_SLOTS; i++) {
        __atomic_store_n(&s_ngp_native_state[i], 0u, __ATOMIC_RELEASE);
    }
    s_ngp_native_write = 0u;
    s_ngp_native_read = 0u;
    s_ngp_run_count = 0u;
    s_write_slot = 0u;
    s_read_slot = 0u;
}

void ili9341_set_scanlines(bool enable) {
    if (s_scanlines_enabled != enable) {
        s_scanlines_enabled = enable;
        s_ngp_hash_reset = true;
    }
}

bool ili9341_get_scanlines(void) {
    return s_scanlines_enabled;
}

void ili9341_set_shimmerless(bool enable) {
    if (s_shimmerless_enabled != enable) {
        s_shimmerless_enabled = enable;
        s_ngp_hash_reset = true;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_shimmerless(void) {
    return s_shimmerless_enabled;
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
        s_ngp_xmap_ready = false;
        ngp_prepare_xmap();
        s_ngp_hash_reset = true;
        s_ngp_last_x0 = 0xFFFFu;
        s_ngp_last_x1 = 0u;
        /* No border clearing here, for either machine. The blitter only writes
         * inside the active CASET window and no longer shares the legacy slot
         * queue, so pushing black rows would fill slots nobody drains and wedge
         * Core 0 after a few scale changes. Both the game start and the
         * pause-menu return go through ili9341_clear_black() immediately before
         * this call, which blanks the whole panel -- letterbox included. */
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
    for (int i = 0; i < NGP_RING_SLOTS; i++) {
        while (__atomic_load_n(&s_ngp_native_state[i], __ATOMIC_ACQUIRE)) {
            tight_loop_contents();
        }
    }
}

bool ili9341_is_frame_busy(void) {
    for (int i = 0; i < NGP_RING_SLOTS; i++) {
        if (__atomic_load_n(&s_ngp_native_state[i], __ATOMIC_ACQUIRE)) return true;
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
