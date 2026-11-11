#include "ili9341.h"
#include "dmg_menu.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"

#include "font_8x8.h"

/* InfoNES palette table reference for backdrop color (weak symbol for non-NES emulators) */
__attribute__((weak)) uint16_t PalTable[64] = {0};

/* ST7789 power-on sequence, identical to the one the PicoNES driver uses on the
 * same panel. Declared before ili9341_init() and defined after it, so the
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
#define SPI_RUN_BAUD   63000000u

/* Framebuffer used by the menu system (320x240 RGB555) */
static uint16_t __aligned(4) s_fb[ILI9341_WIDTH * ILI9341_HEIGHT];

/* ------------------------------------------------------------------ */
/* Per-scanline streaming blitter state (Dual-Core PicoDMG strategy)   */
/* ------------------------------------------------------------------ */
#define NES_LCD_SLOTS 8u
#define NES_ROW_BYTES (ILI9341_WIDTH * 2u)

static uint16_t s_slot_data[NES_LCD_SLOTS][ILI9341_WIDTH];
static uint8_t s_slot_row[NES_LCD_SLOTS];
static volatile uint8_t s_slot_state[NES_LCD_SLOTS]; /* 1 = ready for Core 1 */
static uint8_t s_write_slot = 0u;
static volatile bool s_blitter_running = false;
static volatile bool s_scanlines_enabled = false;
static volatile bool s_grid_enabled = false;
static volatile bool s_is_gamegear = false;
static ili9341_scale_mode_t s_scale_mode = SCALE_FIT;

static uint8_t s_next_row = 0u;
static uint8_t s_field_parity = 0u;
static bool s_sms_interleave = true; /* 0 or 1: interlaced fields to halve SPI bandwidth */
static uint32_t s_row_hash[ILI9341_HEIGHT];
static bool s_bov_frame_rows = false;   /* low-battery overlay: rows emitted this frame */
static uint16_t s_band_xmap[ILI9341_WIDTH];
static bool s_band_xmap_ready = false;
static uint16_t s_sms_xmap[ILI9341_WIDTH];
static uint16_t s_sms_xmap_crop[ILI9341_WIDTH];
static uint16_t s_gg_xmap[ILI9341_WIDTH];
static bool s_shimmerless_enabled = true;
static dmg_scale_weight_t s_sms_xweights[ILI9341_WIDTH];
static dmg_scale_weight_t s_sms_xweights_crop[ILI9341_WIDTH];
static dmg_scale_weight_t s_gg_xweights[ILI9341_WIDTH];
static bool s_sms_xmap_ready = false;
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
     * the ILI9341's 0x28, and they also differ in the 180-degree case (0xA0 vs
     * 0xE8), so hardcoding the ILI9341 table would mis-orient the console panel.
     * That is the 0x60/0xA0 vs 0x28/0xE8 table in dmg_panel.c. */
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
    gpio_set_slew_rate(ILI9341_PIN_SCK, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(ILI9341_PIN_MOSI, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(ILI9341_PIN_SCK, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_drive_strength(ILI9341_PIN_MOSI, GPIO_DRIVE_STRENGTH_8MA);
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
     * registers, and the ST7789 needs a different order (it is a MIPI DBI panel
     * with its own CASET/RASET addressing). Writing the ILI9341 sequence to it
     * produced a blank or mis-scanned screen, so the two are kept apart.
     *
     * Panel and orientation come from /boot.txt via shared_menu, resolved before
     * anything below, so one firmware drives either module. */
    if (dmg_panel_get() == DMG_PANEL_ST7789) {
        st7789_init_sequence();
        spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);
        printf("PicoSMS display: ST7789 MADCTL=0x%02X (%s) %s @ %u Hz\n",
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

    /* Ensure clk_peri is attached to clk_sys at full speed for fast SPI */
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));

    /* Switch SPI to operational baud rate */
    spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);

    nes_prepare_band_xmap();
    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("ILI9341: initialized in landscape 320x240 @ %u Hz (Interlaced Core 1 streaming)\n",
           spi_get_baudrate(ILI9341_SPI));
}

/* ST7789 power-on. Deliberately a short sequence: the ST7789 needs SWRESET,
 * SLPOUT, inversion, COLMOD, MADCTL, NORON and DISPON, in that order. The ILI9341
 * power registers are absent here on purpose -- on a MIPI DBI panel they either
 * do not exist or address unrelated internal state.
 *
 * The delays are the datasheet minimums, not padded: NORON needs 10 ms before
 * the next command and DISPON needs 120 ms before the panel is usable. */
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
     * 0x60 for the ST7789, which is the same value plus MX. */
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

uint16_t *ili9341_get_framebuffer(void) {
    return s_fb;
}

uint16_t *ili9341_get_draw_buffer(void) {
    return s_fb;
}

/* Sends a single scanline to the ILI9341 display */
static void __not_in_flash_func(ili9341_stream_row)(const uint16_t *packed, uint16_t row) {
    if (packed == NULL || row >= ILI9341_HEIGHT) return;

    cs_low();

    dc_cmd();
    uint8_t cmd = CMD_PASET;
    spi_write_blocking(ILI9341_SPI, &cmd, 1);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();

    dc_data();
    uint8_t raset[4] = {
        (uint8_t)(row >> 8), (uint8_t)(row & 0xFF),
        (uint8_t)(row >> 8), (uint8_t)(row & 0xFF)
    };
    spi_write_blocking(ILI9341_SPI, raset, 4);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();

    dc_cmd();
    cmd = CMD_RAMWR;
    spi_write_blocking(ILI9341_SPI, &cmd, 1);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();

    dc_data();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_write16_blocking(ILI9341_SPI, packed, ILI9341_WIDTH);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    cs_high();
}

/* Core 1 background display worker */
static void __not_in_flash_func(nes_blitter_core1)(void) {
    uint8_t read_slot = 0u;

    while (s_blitter_running) {
        if (__atomic_load_n(&s_slot_state[read_slot], __ATOMIC_ACQUIRE)) {
            ili9341_stream_row(s_slot_data[read_slot], s_slot_row[read_slot]);
            __atomic_store_n(&s_slot_state[read_slot], 0u, __ATOMIC_RELEASE);
            read_slot = (uint8_t)((read_slot + 1u) % NES_LCD_SLOTS);
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

    uint16_t *dst = s_slot_data[slot];
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
        dst[x] = c;
    }
    s_slot_row[slot] = row;

    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

void ili9341_stream_nes_line(int line, const uint16_t *rgb555_256) {
    if (!s_blitter_running || line < 0 || line >= ILI9341_HEIGHT) return;

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
    if (!s_blitter_running) return;

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
    if (s_sms_interleave) { s_field_parity ^= 1u; }
}

static void sms_prepare_xmaps(void);

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

void ili9341_blitter_start(void) {
    if (s_blitter_running) return;
    nes_prepare_band_xmap();
    sms_prepare_xmaps();
    ili9341_clear_black();
    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    for (int i = 0; i < ILI9341_HEIGHT; i++) {
        s_row_hash[i] = 0u;
    }
    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        __atomic_store_n(&s_slot_state[i], 0u, __ATOMIC_RELEASE);
    }
    s_write_slot = 0u;
    s_next_row = 0u;
    s_field_parity = 0u;
    s_current_backdrop555 = PalTable[0];
    s_blitter_running = true;
    multicore_reset_core1();
    multicore_launch_core1(nes_blitter_core1);
}

void ili9341_blitter_stop(void) {
    if (!s_blitter_running) return;
    /* Wait for Core 1 to finish publishing slots */
    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        while (__atomic_load_n(&s_slot_state[i], __ATOMIC_ACQUIRE)) {
            tight_loop_contents();
        }
    }
    s_blitter_running = false;
    multicore_reset_core1();
}

static inline uint16_t ili9341_grid_darken(uint16_t c) {
    /* ~14% darker: masks the pixel cell borders (LCD grid effect). */
    uint32_t r = (uint32_t)(c >> 11);
    uint32_t g = (uint32_t)((c >> 5) & 0x3Fu);
    uint32_t b = (uint32_t)(c & 0x1Fu);
    r = (r * 220u) >> 8;
    g = (g * 220u) >> 8;
    b = (b * 220u) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void sms_prepare_xmaps(void) {
    if (s_sms_xmap_ready) return;

    /* SMS normal: 256 pixels */
    /* SMS crop (Sonic): pixels 8..255 (248 pixels) */
    /* GG normal: pixels 48..207 (160 pixels) */

    memset(s_sms_xweights, 0, sizeof(s_sms_xweights));
    memset(s_sms_xweights_crop, 0, sizeof(s_sms_xweights_crop));
    memset(s_gg_xweights, 0, sizeof(s_gg_xweights));

    static dmg_scale_weight_t temp[ILI9341_WIDTH];

    if (dmg_is_gb_mode()) {
        /* Master System within DMG window (x=45..310, width 266) */
        if (s_scale_mode == SCALE_INTEGER) {
            /* 256 centered on 266: 5px borders (x=50..305) */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 50 || x >= 306) {
                    s_sms_xmap[x] = 0xFFFFu;
                    s_sms_xmap_crop[x] = 0xFFFFu;
                } else {
                    uint16_t ax = x - 50;
                    s_sms_xmap[x] = ax;
                    s_sms_xmap_crop[x] = 8u + (uint16_t)(((uint32_t)ax * 248u) / 256u);
                }
            }
        } else {
            /* SCALE_FIT & SCALE_STRETCH: Scale across 266px window (x=45..310) */
            dmg_init_shimmerless_weights(256, 266, temp);
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 45 || x >= 311) {
                    s_sms_xmap[x] = 0xFFFFu;
                } else {
                    uint16_t ax = (uint16_t)(((uint32_t)(x - 45) * 256u) / 266u);
                    s_sms_xmap[x] = ax;
                    s_sms_xweights[x].src_idx = temp[x - 45].src_idx;
                    s_sms_xweights[x].weight1 = temp[x - 45].weight1;
                }
            }
            dmg_init_shimmerless_weights(248, 266, temp);
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 45 || x >= 311) {
                    s_sms_xmap_crop[x] = 0xFFFFu;
                } else {
                    uint16_t ax = 8u + (uint16_t)(((uint32_t)(x - 45) * 248u) / 266u);
                    s_sms_xmap_crop[x] = ax;
                    s_sms_xweights_crop[x].src_idx = 8u + temp[x - 45].src_idx;
                    s_sms_xweights_crop[x].weight1 = temp[x - 45].weight1;
                }
            }
        }

        /* Game Gear within DMG window (x=45..310, width 266) */
        if (s_scale_mode == SCALE_INTEGER) {
            /* 160 centered on 266: 53px borders inside window (x=98..257) */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 98 || x >= 258) {
                    s_gg_xmap[x] = 0xFFFFu;
                } else {
                    s_gg_xmap[x] = 48u + (uint16_t)(x - 98);
                }
            }
        } else {
            /* SCALE_FIT & SCALE_STRETCH: Scale 160 across 266px window (x=45..310) */
            dmg_init_shimmerless_weights(160, 266, temp);
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 45 || x >= 311) {
                    s_gg_xmap[x] = 0xFFFFu;
                } else {
                    s_gg_xmap[x] = 48u + (uint16_t)(((uint32_t)(x - 45) * 160u) / 266u);
                    s_gg_xweights[x].src_idx = 48u + temp[x - 45].src_idx;
                    s_gg_xweights[x].weight1 = temp[x - 45].weight1;
                }
            }
        }
    } else {
        if (s_scale_mode == SCALE_INTEGER || s_scale_mode == SCALE_FIT) {
            /* SMS: 256 centered on 320 (32px left/right borders) */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 32 || x >= 288) {
                    s_sms_xmap[x] = 0xFFFFu;
                    s_sms_xmap_crop[x] = 0xFFFFu;
                } else {
                    uint16_t ax = x - 32;
                    s_sms_xmap[x] = ax;
                    s_sms_xmap_crop[x] = 8u + (uint16_t)(((uint32_t)ax * 248u) / 256u);
                }
            }
        } else {
            /* SCALE_STRETCH: 320 full screen */
            dmg_init_shimmerless_weights(256, 320, s_sms_xweights);
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                s_sms_xmap[x] = (uint16_t)(((uint32_t)x * 256u) / (uint32_t)ILI9341_WIDTH);
            }
            dmg_init_shimmerless_weights(248, 320, temp);
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                s_sms_xmap_crop[x] = 8u + (uint16_t)(((uint32_t)x * 248u) / (uint32_t)ILI9341_WIDTH);
                s_sms_xweights_crop[x].src_idx = 8u + temp[x].src_idx;
                s_sms_xweights_crop[x].weight1 = temp[x].weight1;
            }
        }

        /* Game Gear */
        if (s_scale_mode == SCALE_INTEGER) {
            /* 160 centered on 320 (80px borders) */
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 80 || x >= 240) {
                    s_gg_xmap[x] = 0xFFFFu;
                } else {
                    s_gg_xmap[x] = 48u + (uint16_t)(x - 80);
                }
            }
        } else if (s_scale_mode == SCALE_FIT) {
            /* 266 centered on 320 (27px borders) */
            dmg_init_shimmerless_weights(160, 266, temp);
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                if (x < 27 || x >= 293) {
                    s_gg_xmap[x] = 0xFFFFu;
                } else {
                    s_gg_xmap[x] = 48u + (uint16_t)(((uint32_t)(x - 27) * 160u) / 266u);
                    s_gg_xweights[x].src_idx = 48u + temp[x - 27].src_idx;
                    s_gg_xweights[x].weight1 = temp[x - 27].weight1;
                }
            }
        } else {
            /* SCALE_STRETCH: 320 full screen */
            dmg_init_shimmerless_weights(160, 320, temp);
            for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
                s_gg_xmap[x] = 48u + (uint16_t)(((uint32_t)x * 160u) / (uint32_t)ILI9341_WIDTH);
                s_gg_xweights[x].src_idx = 48u + temp[x].src_idx;
                s_gg_xweights[x].weight1 = temp[x].weight1;
            }
        }
    }

    s_sms_xmap_ready = true;
}

static volatile bool s_fps_enabled = false;
static char s_fps_text[16] = "60";

static inline char get_font_slice(char c, int row) {
    if (c < 32 || c >= (32 + 95)) return 0;
    return font_8x8[(c - 32) + row * 95];
}

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

static void __not_in_flash_func(sms_band_push)(const uint8_t *src_buf, const uint16_t *pal, const uint16_t *xmap, uint8_t row) {
    if (row >= ILI9341_HEIGHT) return;

    uint16_t tmp[ILI9341_WIDTH];
    const bool y_grid = s_grid_enabled && ((row & 1u) == 1u);
    const bool dim_scanlines = s_scanlines_enabled && ((row & 1u) == 1u);
    const bool do_blend = s_shimmerless_enabled && (s_scale_mode != SCALE_INTEGER);
    const dmg_scale_weight_t *xweights = (xmap == s_gg_xmap) ? s_gg_xweights : (xmap == s_sms_xmap_crop ? s_sms_xweights_crop : s_sms_xweights);

    if (src_buf && pal && xmap) {
        if (do_blend && xweights) {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t map_idx = xmap[x];
                uint16_t c;
                if (map_idx == 0xFFFFu) {
                    c = 0x0000;
                } else {
                    uint8_t w = xweights[x].weight1;
                    uint16_t c0 = pal[src_buf[xweights[x].src_idx] & 0x1Fu];
                    if (w > 0u) {
                        uint16_t c1 = pal[src_buf[xweights[x].src_idx + 1u] & 0x1Fu];
                        c = dmg_blend_rgb565(c0, c1, w);
                    } else {
                        c = c0;
                    }
                    if (dim_scanlines) {
                        c = (c >> 1) & 0x7BEF;
                    } else if (s_grid_enabled) {
                        if (y_grid || ((x & 1u) == 1u)) {
                            c = ili9341_grid_darken(c);
                        }
                    }
                }
                tmp[x] = c;
            }
        } else {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t map_idx = xmap[x];
                uint16_t c;
                if (map_idx == 0xFFFFu) {
                    c = 0x0000; /* Black border */
                } else {
                    uint8_t pal_idx = src_buf[map_idx] & 0x1Fu;
                    c = pal[pal_idx];
                    if (dim_scanlines) {
                        c = (c >> 1) & 0x7BEF;
                    } else if (s_grid_enabled) {
                        if (y_grid || ((x & 1u) == 1u)) {
                            c = ili9341_grid_darken(c);
                        }
                    }
                }
                tmp[x] = c;
            }
        }
    } else {
        memset(tmp, 0, sizeof(tmp));
    }

    /* Direct FPS Overlay on lines 8..15 */
    if (s_fps_enabled && row >= 8 && row < 16) {
        int r = (int)row - 8;
        int x_pos = dmg_is_gb_mode() ? (45 + 8) : 8;
        int max_x = dmg_is_gb_mode() ? 311 : ILI9341_WIDTH;
        for (int i = 0; s_fps_text[i] != '\0' && (x_pos + 8) < max_x; i++) {
            char slice = get_font_slice(s_fps_text[i], r);
            for (int bit = 0; bit < 8; bit++) {
                if (slice & (1 << bit)) {
                    tmp[x_pos + bit] = 0x07E0; /* Bright Green */
                } else {
                    tmp[x_pos + bit] = 0x0000; /* Black box */
                }
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
                tmp[hud_x + i] = c;
            }
        }
    }

    /* Low-battery overlay. Stamped before the hash, so the row is resent
     * whenever the overlay appears, changes or hides. */
    if (src_buf) {
        dmg_batt_overlay_mark_row(row);
        s_bov_frame_rows = true;
    }
    dmg_batt_overlay_stamp_row(row, tmp, 0, ILI9341_WIDTH, false);

    /* Dirty-row skip */
    uint32_t h = 2166136261u;
    for (uint16_t i = 0; i < ILI9341_WIDTH; i++) {
        h = (h ^ tmp[i]) * 16777619u;
    }
    if (h == s_row_hash[row]) {
        return;
    }
    s_row_hash[row] = h;

    const uint8_t slot = s_write_slot;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    memcpy(s_slot_data[slot], tmp, sizeof(tmp));
    s_slot_row[slot] = row;

    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

void ili9341_stream_sms_line(int line, const uint8_t *buffer, const uint16_t *pal, bool crop_left) {
    if (!s_blitter_running || !buffer || !pal) return;
    if (!s_sms_xmap_ready) sms_prepare_xmaps();

    if (s_is_gamegear) {
        /* GG active lines in smsplus: 24..167 (144 lines) */
        if (line < 24 || line >= 168) return;
        const int l0 = line - 24; /* 0..143 */

        if (s_scale_mode == SCALE_INTEGER) {
            int panel_row = l0 + 48; /* Centered: 48..191 */
            if (panel_row >= 0 && panel_row < ILI9341_HEIGHT) {
                if (!s_sms_interleave || ((uint8_t)panel_row & 1u) == s_field_parity) {
                    sms_band_push(buffer, pal, s_gg_xmap, (uint8_t)panel_row);
                }
            }
        } else {
            /* FIT (10:9 -> 266x240) and STRETCH (320x240): 144 lines scaled to 240 */
            int r0 = (l0 * 240) / 144;
            int r1 = ((l0 + 1) * 240) / 144;
            if (r1 > ILI9341_HEIGHT) r1 = ILI9341_HEIGHT;
            for (int r = r0; r < r1; r++) {
                if (!s_sms_interleave || ((uint8_t)r & 1u) == s_field_parity) {
                    sms_band_push(buffer, pal, s_gg_xmap, (uint8_t)r);
                }
            }
        }
    } else {
        /* Master System: active lines 0..191 (192 lines) */
        if (line < 0 || line >= 192) return;
        const uint16_t *xmap = crop_left ? s_sms_xmap_crop : s_sms_xmap;

        if (s_scale_mode == SCALE_INTEGER) {
            /* 192 lines centered: 24..215 */
            int panel_row = line + 24;
            if (panel_row >= 0 && panel_row < ILI9341_HEIGHT) {
                if (!s_sms_interleave || ((uint8_t)panel_row & 1u) == s_field_parity) {
                    sms_band_push(buffer, pal, xmap, (uint8_t)panel_row);
                }
            }
        } else {
            /* FIT (256x240) and STRETCH (320x240): 192 lines scaled to 240 */
            int r0 = (line * 240) / 192;
            int r1 = ((line + 1) * 240) / 192;
            if (r1 > ILI9341_HEIGHT) r1 = ILI9341_HEIGHT;
            for (int r = r0; r < r1; r++) {
                if (!s_sms_interleave || ((uint8_t)r & 1u) == s_field_parity) {
                    sms_band_push(buffer, pal, xmap, (uint8_t)r);
                }
            }
        }
    }
}

void ili9341_stream_sms_frame_end(void) {
    if (!s_blitter_running) return;

    if (s_scale_mode == SCALE_INTEGER) {
        static bool s_last_fps_active = false;
        if (s_fps_enabled || s_last_fps_active) {
            for (uint8_t r = 8; r < 16; r++) {
                if ((r & 1u) == s_field_parity) {
                    sms_band_push(NULL, NULL, NULL, r);
                }
            }
            s_last_fps_active = s_fps_enabled;
        }

        /* Only the HUD rows the image does not cover: pushing black over image
         * rows (SMS integer covers 24..215, so all of 180..199) blanked the game
         * under the HUD every frame. */
        const uint8_t img_y0 = s_is_gamegear ? 48u : 24u;
        const uint8_t img_y1 = s_is_gamegear ? 192u : 216u;
        static bool s_last_hud_active = false;
        bool hud_act = dmg_volume_hud_is_active();
        if (hud_act || s_last_hud_active) {
            for (uint8_t r = 180; r < 200; r++) {
                if (r >= img_y0 && r < img_y1) continue;
                if (!s_sms_interleave || ((r & 1u) == s_field_parity)) {
                    sms_band_push(NULL, NULL, NULL, r);
                }
            }
            s_last_hud_active = hud_act;
        }
    }

    /* Low-battery overlay rows outside the image: black + overlay */
    if (s_bov_frame_rows) {
        s_bov_frame_rows = false;
        const uint32_t m = dmg_batt_overlay_frame_rows();
        const uint16_t y0 = dmg_batt_overlay_y0();
        for (uint16_t i = 0; i < dmg_batt_overlay_h(); i++) {
            if (m & (1u << i)) {
                s_row_hash[y0 + i] = 0xFFFFFFFFu;   /* force: refresh after a screen clear */
                sms_band_push(NULL, NULL, NULL, (uint8_t)(y0 + i));
            }
        }
    }

    if (s_sms_interleave) { s_field_parity ^= 1u; }
}

void ili9341_set_interleave(bool enable) {
    if (s_sms_interleave != enable) {
        s_sms_interleave = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_interleave(void) {
    return s_sms_interleave;
}

bool ili9341_is_active_sms_line(int line) {
    if (!s_sms_interleave) return true;
    int panel_row = s_is_gamegear ? ((line - 24) + 48) : (line + 24);
    return (((uint8_t)panel_row & 1u) == s_field_parity);
}

void ili9341_set_scanlines(bool enable) {
    if (s_scanlines_enabled != enable) {
        s_scanlines_enabled = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0u;
        }
    }
}

bool ili9341_get_scanlines(void) {
    return s_scanlines_enabled;
}

void ili9341_set_grid(bool enable) {
    if (s_grid_enabled != enable) {
        s_grid_enabled = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0u;
        }
    }
}

bool ili9341_get_grid(void) {
    return s_grid_enabled;
}

void ili9341_set_is_gamegear(bool is_gg) {
    if (s_is_gamegear != is_gg) {
        s_is_gamegear = is_gg;
        s_sms_xmap_ready = false;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0u;
        }
    }
}

bool ili9341_get_is_gamegear(void) {
    return s_is_gamegear;
}

void ili9341_set_shimmerless(bool enable) {
    if (s_shimmerless_enabled != enable) {
        s_shimmerless_enabled = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0u;
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
        s_sms_xmap_ready = false;
        nes_prepare_band_xmap();
        sms_prepare_xmaps();
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0u;
        }
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
    if (!s_blitter_running) return;
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
