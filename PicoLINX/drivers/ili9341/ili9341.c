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
#define SPI_RUN_BAUD   72000000u

/* Framebuffer used by the menu system (320x240 RGB555) */
static uint16_t __aligned(4) s_fb[ILI9341_WIDTH * ILI9341_HEIGHT];

/* ------------------------------------------------------------------ */
/* Per-scanline streaming blitter state (Dual-Core PicoDMG strategy)   */
/* ------------------------------------------------------------------ */
#define NES_LCD_SLOTS 224u
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

static void st7789_init_sequence(void);
static void ili9341_init_sequence(void);

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

/* The panel is a runtime choice, not a compile-time constant. dmg_panel.c reads
 * /boot.txt (ST7789= / MADCTL= / ROTATE= / INVERT=) and hands the bootloader
 * the same decision, so loader and emulator agree by construction.
 *
 * The MADCTL table was established per panel on real hardware:
 *   ILI9341 (PicoDMG)  landscape 0x28 / 180 deg 0xE8
 *   ST7789 (Core2350B)  landscape 0x60 / 180 deg 0xA0
 * The ST7789 needs MX set (0x60) and reads colour order with BGR clear, the
 * opposite of the ILI9341 with the same bit. 0x60 is measured, not derived:
 * no datasheet declares the mirror bits, they depend on how the controller is
 * bonded to the glass.
 *
 * 180 deg is XOR 0xC0, not OR 0xC0: 0x60 already has MX set, so OR would only
 * add MY and hand back the ILI9341's value, i.e. the same image mirrored on the
 * wrong axis. On the ILI9341 (0x28 has neither MX nor MY) the two happen to
 * coincide, which is why the OR bug survived and only showed on the new panel. */
static void apply_madctl(void) {
    write_cmd(CMD_MADCTL);
    write_data(dmg_panel_get_madctl());
}

void ili9341_set_rotation(uint8_t rotation) {
    /* The rotation index (1 = landscape, 3 = landscape inverted) is kept for
     * callers that only care about orientation; the MADCTL itself now comes
     * from shared_menu. */
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

void ili9341_init(void) {
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

    /* Panel-specific power-on. The ILI9341 needs the full gamma/voltage/
     * framerate block below; the ST7789 ignores most of it and has its own
     * minimal sequence, so they cannot share one. */
    if (dmg_panel_get() == DMG_PANEL_ST7789) {
        st7789_init_sequence();
    } else {
        ili9341_init_sequence();
    }

    /* Switch SPI to operational baud rate */
    spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);

    nes_prepare_band_xmap();
    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("Display: %s initialized, landscape 320x240, MADCTL=0x%02X, %s, SPI %u Hz (Core 1 streaming)\n",
           dmg_panel_get() == DMG_PANEL_ST7789 ? "ST7789" : "ILI9341",
           (unsigned)dmg_panel_get_madctl(),
           dmg_panel_get_invert() ? "INVON" : "INVOFF",
           spi_get_baudrate(ILI9341_SPI));
}

/* ST7789 (Waveshare Core2350B). Same sequence the bootloader and the other
 * emulators use: SWRESET -> SLPOUT -> INVON/INVOFF -> COLMOD -> MADCTL ->
 * addr window -> NORON -> DISPON. */
/* Adapter for dmg_panel_st7789_timing(), which takes one combined
 * command+parameter primitive, while this driver keeps write_cmd() and
 * write_data_buf() separate. */
static void panel_timing_write(uint8_t cmd, const uint8_t *data, size_t len) {
    write_cmd(cmd);
    if (data != NULL && len > 0) write_data_buf(data, len);
}

static void st7789_init_sequence(void) {
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

/* ILI9341: the verified pico-bootLoader profile, unchanged. */
static void ili9341_init_sequence(void) {
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

/* Lynx FPS overlay band: rows 69..76 are streamed in Fit, Stretch and Integer */
#define LYNX_FPS_ROW_TOP 69
#define LYNX_FPS_ROW_END 77

void ili9341_set_fps_overlay(bool enable, const char *fps_str) {
    if (s_fps_enabled != enable) {
        s_fps_enabled = enable;
        for (int r = 8; r < 16; r++) {
            s_row_hash[r] = 0u;
        }
        for (int r = LYNX_FPS_ROW_TOP; r < LYNX_FPS_ROW_END; r++) {
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
            for (int r = LYNX_FPS_ROW_TOP; r < LYNX_FPS_ROW_END; r++) {
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

/* Sends a single scanline to the ILI9341 display in a single transaction */
static void __not_in_flash_func(ili9341_stream_row)(const uint8_t *packed, uint16_t row) {
    if (packed == NULL || row >= ILI9341_HEIGHT) return;

    cs_low();

    /* 1. Set row address (PASET) */
    dc_cmd();
    uint8_t cmd_paset = CMD_PASET;
    spi_write_blocking(ILI9341_SPI, &cmd_paset, 1);

    dc_data();
    uint8_t raset[4] = {
        (uint8_t)(row >> 8), (uint8_t)(row & 0xFF),
        (uint8_t)(row >> 8), (uint8_t)(row & 0xFF)
    };
    spi_write_blocking(ILI9341_SPI, raset, 4);

    /* 2. Start RAM write (RAMWR) */
    dc_cmd();
    uint8_t cmd_ramwr = CMD_RAMWR;
    spi_write_blocking(ILI9341_SPI, &cmd_ramwr, 1);

    /* 3. Send scanline pixel data */
    dc_data();
    spi_write_blocking(ILI9341_SPI, packed, ILI9341_WIDTH * 2u);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();

    cs_high();
}

/* Core 1 background display worker */
static void __not_in_flash_func(nes_blitter_core1)(void) {
    while (1) {
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
static uint16_t s_lut_ngp_to_rgb565[4096];
static bool s_ngp_lut_ready = false;
static volatile bool s_grid_effect = false;

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

static void ngp_prepare_xmap(void) {
    if (s_ngp_xmap_ready) return;
    if (s_scale_mode == SCALE_INTEGER) {
        /* 160x152 centered (80 px borders left and right) */
        for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
            if (x < 80 || x >= 240) {
                s_ngp_xmap[x] = 0xFFFFu;
            } else {
                s_ngp_xmap[x] = (uint16_t)(x - 80);
            }
        }
    } else if (s_scale_mode == SCALE_FIT) {
        /* Proportional ~1.05:1 PAR: 253 px wide, centered with 34 px borders */
        for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
            if (x < 34 || x >= 287) {
                s_ngp_xmap[x] = 0xFFFFu;
            } else {
                s_ngp_xmap[x] = (uint16_t)(((uint32_t)(x - 34) * 160u) / 253u);
            }
        }
    } else {
        /* Full screen stretch 320x240: 160 px -> 320 px (2x duplicate) */
        for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
            s_ngp_xmap[x] = (uint16_t)(x >> 1);
        }
    }
    s_ngp_xmap_ready = true;
}

static void __not_in_flash_func(ngp_band_push)(const uint16_t *ngp_line_160, uint8_t row) {
    if (!s_blitter_active || row >= ILI9341_HEIGHT) return;

    if (ngp_line_160 == NULL) {
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

    /* Fast 32-bit FNV hash over the 160 uint16 words (80 uint32 words) */
    const uint32_t *src32 = (const uint32_t *)ngp_line_160;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 80; i++) {
        h = (h ^ src32[i]) * 16777619u;
    }
    if (s_grid_effect && (row & 1u)) {
        h ^= 0x5A5A5A5Au;
    }

    if (h == s_row_hash[row] && (!s_fps_enabled || row < 8 || row >= 16)) return;
    s_row_hash[row] = h;

    ngp_prepare_xmap();
    const uint16_t *xmap = s_ngp_xmap;

    const uint8_t slot = s_write_slot;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    uint8_t *dst = s_slot_data[slot];
    const bool row_dim = s_grid_effect && (row & 1u);
    const bool grid_on = s_grid_effect;

    uint16_t *dst16 = (uint16_t *)dst;
    if (!grid_on) {
        for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t map_idx = xmap[x];
            if (__builtin_expect(map_idx != 0xFFFFu, 1)) {
                uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[map_idx] & 0x0FFF];
                dst16[x] = __builtin_bswap16(c);
            } else {
                dst16[x] = 0;
            }
        }
    } else {
        for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t map_idx = xmap[x];
            if (__builtin_expect(map_idx != 0xFFFFu, 1)) {
                uint16_t c = s_lut_ngp_to_rgb565[ngp_line_160[map_idx] & 0x0FFF];
                if (row_dim || (x & 1u)) {
                    c = ili9341_grid_darken(c);
                }
                dst16[x] = __builtin_bswap16(c);
            } else {
                dst16[x] = 0;
            }
        }
    }

    /* Direct FPS overlay on lines 8..15 */
    if (s_fps_enabled && row >= 8 && row < 16) {
        int r = (int)row - 8;
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

    s_slot_row[slot] = row;
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

bool __not_in_flash_func(ili9341_is_active_ngp_line)(int line) {
    if (!s_blitter_active || line < 0 || line >= 152) return false;
    if (!s_ngp_interleave) return true;
    if (s_scale_mode == SCALE_INTEGER) {
        uint8_t row = (uint8_t)(44 + line);
        if (row >= ILI9341_HEIGHT) return false;
        return ((row & 1u) == s_field_parity);
    } else {
        init_ngp_stretch_tables();
        return (s_ngp_stretch_row_count[s_field_parity][line] > 0);
    }
}

void __not_in_flash_func(ili9341_stream_ngp_line)(int line, const uint16_t *ngp_line_160) {
    if (!s_blitter_active || line < 0 || line >= 152 || ngp_line_160 == NULL) return;
    init_ngp_lut();

    if (s_scale_mode == SCALE_INTEGER) {
        /* Centered 160x152: row = 44 + line */
        uint8_t row = (uint8_t)(44 + line);
        if (row >= ILI9341_HEIGHT) return;
        if (s_ngp_interleave && ((row & 1u) != s_field_parity)) return;
        ngp_band_push(ngp_line_160, row);
    } else {
        /* Fit or Stretch (both map 152 lines -> 240 rows vertically) */
        init_ngp_stretch_tables();
        if (s_ngp_interleave) {
            uint8_t cnt = s_ngp_stretch_row_count[s_field_parity][line];
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t row = s_ngp_stretch_rows[s_field_parity][line][i];
                if (row < ILI9341_HEIGHT) {
                    ngp_band_push(ngp_line_160, row);
                }
            }
        } else {
            uint8_t cnt = s_ngp_prog_row_count[line];
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t row = s_ngp_prog_rows[line][i];
                if (row < ILI9341_HEIGHT) {
                    ngp_band_push(ngp_line_160, row);
                }
            }
        }
    }
}

void __not_in_flash_func(ili9341_stream_ngp_frame_end)(void) {
    if (!s_blitter_active) return;

    if (s_fps_enabled && s_scale_mode == SCALE_INTEGER) {
        for (uint8_t row = 8; row < 16; row++) {
            if (!s_ngp_interleave || (row & 1u) == s_field_parity) {
                const uint8_t slot = s_write_slot;
                while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
                    tight_loop_contents();
                }
                uint8_t *dst = s_slot_data[slot];
                memset(dst, 0, ILI9341_WIDTH * 2);
                int r = (int)row - 8;
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
                s_slot_row[slot] = row;
                __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
                s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
            }
        }
    }

    if (s_ngp_interleave) {
        s_field_parity ^= 1u;
    }
}

/* =========================================================================
 * Atari Lynx / Handy Streaming Support (IDX8 160x102 + palette RGB565)
 * ========================================================================= */
#define LYNX_SCREEN_W 160
#define LYNX_SCREEN_H 102

/* Fit keeps the native 160x102 aspect: uniform 2x -> 320x204 centered (18 px top/bottom) */
#define LYNX_FIT_SCALE 2
#define LYNX_FIT_TOP   ((ILI9341_HEIGHT - (LYNX_SCREEN_H * LYNX_FIT_SCALE)) / 2)

static uint16_t s_lynx_xmap[ILI9341_WIDTH];
static bool s_lynx_xmap_ready = false;
static bool s_shimmerless_enabled = true;
static dmg_scale_weight_t s_lynx_xweights[ILI9341_WIDTH];

static void lynx_prepare_xmap(void) {
    if (s_lynx_xmap_ready) return;
    int disp_x = 0;
    int disp_w = ILI9341_WIDTH;
    if (dmg_is_gb_mode()) {
        if (s_scale_mode == SCALE_INTEGER) {
            disp_x = 98;
            disp_w = 160;
        } else {
            disp_x = 45;
            disp_w = 266;
        }
    } else {
        if (s_scale_mode == SCALE_INTEGER) {
            disp_x = 80;
            disp_w = 160;
        } else {
            disp_x = 0;
            disp_w = ILI9341_WIDTH;
        }
    }

    if (s_scale_mode == SCALE_INTEGER) {
        for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
            if (x < disp_x || x >= disp_x + disp_w) {
                s_lynx_xmap[x] = 0xFFFFu;
                s_lynx_xweights[x].src_idx = 0xFFFFu;
                s_lynx_xweights[x].weight1 = 0;
            } else {
                int src = x - disp_x;
                s_lynx_xmap[x] = (uint16_t)src;
                s_lynx_xweights[x].src_idx = (uint16_t)src;
                s_lynx_xweights[x].weight1 = 0;
            }
        }
    } else {
        dmg_scale_weight_t temp[ILI9341_WIDTH];
        dmg_init_shimmerless_weights((uint16_t)LYNX_SCREEN_W, (uint16_t)disp_w, temp);
        for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
            if (x < disp_x || x >= disp_x + disp_w) {
                s_lynx_xmap[x] = 0xFFFFu;
                s_lynx_xweights[x].src_idx = 0xFFFFu;
                s_lynx_xweights[x].weight1 = 0;
            } else {
                int lx = x - disp_x;
                s_lynx_xmap[x] = (uint16_t)(((uint32_t)lx * LYNX_SCREEN_W) / (uint32_t)disp_w);
                s_lynx_xweights[x] = temp[lx];
            }
        }
    }
    s_lynx_xmap_ready = true;
}

static uint8_t s_lynx_stretch_rows[2][LYNX_SCREEN_H][4];
static uint8_t s_lynx_stretch_row_count[2][LYNX_SCREEN_H];
static uint8_t s_lynx_prog_rows[LYNX_SCREEN_H][4];
static uint8_t s_lynx_prog_row_count[LYNX_SCREEN_H];

static uint8_t s_lynx_fit_gb_rows[2][LYNX_SCREEN_H][4];
static uint8_t s_lynx_fit_gb_row_count[2][LYNX_SCREEN_H];
static uint8_t s_lynx_fit_gb_prog_rows[LYNX_SCREEN_H][4];
static uint8_t s_lynx_fit_gb_prog_row_count[LYNX_SCREEN_H];
static bool s_lynx_stretch_inited = false;

static void init_lynx_stretch_tables(void) {
    if (s_lynx_stretch_inited) return;
    memset(s_lynx_stretch_row_count, 0, sizeof(s_lynx_stretch_row_count));
    memset(s_lynx_prog_row_count, 0, sizeof(s_lynx_prog_row_count));
    for (int r = 0; r < ILI9341_HEIGHT; r++) {
        int s = (r * LYNX_SCREEN_H) / ILI9341_HEIGHT;
        int parity = r & 1;
        uint8_t cnt = s_lynx_stretch_row_count[parity][s];
        if (cnt < 4) {
            s_lynx_stretch_rows[parity][s][cnt] = (uint8_t)r;
            s_lynx_stretch_row_count[parity][s] = cnt + 1;
        }
        uint8_t pcnt = s_lynx_prog_row_count[s];
        if (pcnt < 4) {
            s_lynx_prog_rows[s][pcnt] = (uint8_t)r;
            s_lynx_prog_row_count[s] = pcnt + 1;
        }
    }
    /* GB_MODE Fit tables (170 rows centered vertically at y=35) */
    memset(s_lynx_fit_gb_row_count, 0, sizeof(s_lynx_fit_gb_row_count));
    memset(s_lynx_fit_gb_prog_row_count, 0, sizeof(s_lynx_fit_gb_prog_row_count));
    for (int y = 0; y < 170; y++) {
        int r = y + 35;
        int s = (y * LYNX_SCREEN_H) / 170;
        int parity = r & 1;
        uint8_t cnt = s_lynx_fit_gb_row_count[parity][s];
        if (cnt < 4) {
            s_lynx_fit_gb_rows[parity][s][cnt] = (uint8_t)r;
            s_lynx_fit_gb_row_count[parity][s] = cnt + 1;
        }
        uint8_t pcnt = s_lynx_fit_gb_prog_row_count[s];
        if (pcnt < 4) {
            s_lynx_fit_gb_prog_rows[s][pcnt] = (uint8_t)r;
            s_lynx_fit_gb_prog_row_count[s] = pcnt + 1;
        }
    }
    s_lynx_stretch_inited = true;
}

static void __not_in_flash_func(lynx_band_push)(const uint8_t *indexed_lynx, const uint16_t *palette_rgb565, uint8_t row) {
    if (!s_blitter_active || row >= ILI9341_HEIGHT) return;

    if (indexed_lynx == NULL) {
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

    /* Fast 32-bit FNV hash over the 160 IDX8 bytes (40 uint32 words) */
    const uint32_t *src32 = (const uint32_t *)indexed_lynx;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 40; i++) {
        h = (h ^ src32[i]) * 16777619u;
    }
    if (s_grid_effect && (row & 1u)) {
        h ^= 0x5A5A5A5Au;
    }
    /* low-battery overlay: resend the row when it appears, changes or hides */
    dmg_batt_overlay_mark_row(row);
    s_bov_frame_rows = true;
    h ^= dmg_batt_overlay_row_salt(row);

    if (h == s_row_hash[row] && (!s_fps_enabled || row < LYNX_FPS_ROW_TOP || row >= LYNX_FPS_ROW_END)) return;
    s_row_hash[row] = h;

    lynx_prepare_xmap();
    const uint16_t *xmap = s_lynx_xmap;

    const uint8_t slot = s_write_slot;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    uint8_t *dst = s_slot_data[slot];
    const bool row_dim = s_grid_effect && (row & 1u);
    const bool grid_on = s_grid_effect;

    uint16_t *dst16 = (uint16_t *)dst;
    const bool do_blend = s_shimmerless_enabled && (s_scale_mode != SCALE_INTEGER);

    if (!do_blend) {
        if (__builtin_expect(!dmg_is_gb_mode() && !grid_on && s_scale_mode != SCALE_INTEGER, 1)) {
            /* Fast path: Fit/Stretch map 160 source px 1:1 onto duplicated dest pairs (2x horizontal) */
            const uint32_t *src32_fast = (const uint32_t *)indexed_lynx;
            uint32_t *dst32 = (uint32_t *)dst;
            for (int i = 0; i < (LYNX_SCREEN_W / 4); i++) {
                uint32_t w = src32_fast[i];
                uint16_t c0 = __builtin_bswap16(palette_rgb565[(uint8_t)(w & 0xFFu)]);
                uint16_t c1 = __builtin_bswap16(palette_rgb565[(uint8_t)((w >> 8) & 0xFFu)]);
                uint16_t c2 = __builtin_bswap16(palette_rgb565[(uint8_t)((w >> 16) & 0xFFu)]);
                uint16_t c3 = __builtin_bswap16(palette_rgb565[(uint8_t)((w >> 24) & 0xFFu)]);
                dst32[4 * i + 0] = (uint32_t)c0 | ((uint32_t)c0 << 16);
                dst32[4 * i + 1] = (uint32_t)c1 | ((uint32_t)c1 << 16);
                dst32[4 * i + 2] = (uint32_t)c2 | ((uint32_t)c2 << 16);
                dst32[4 * i + 3] = (uint32_t)c3 | ((uint32_t)c3 << 16);
            }
        } else if (!grid_on) {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t map_idx = xmap[x];
                if (__builtin_expect(map_idx != 0xFFFFu, 1)) {
                    uint16_t c = palette_rgb565[indexed_lynx[map_idx] & 0xFFu];
                    dst16[x] = __builtin_bswap16(c);
                } else {
                    dst16[x] = 0;
                }
            }
        } else if (!dmg_is_gb_mode() && s_scale_mode != SCALE_INTEGER) {
            /* Grid fast path: Fit/Stretch 2x horizontal in full screen mode */
            const uint32_t *src32_fast = (const uint32_t *)indexed_lynx;
            uint32_t *dst32 = (uint32_t *)dst;
            for (int i = 0; i < (LYNX_SCREEN_W / 4); i++) {
                uint32_t w = src32_fast[i];
                for (int b = 0; b < 4; b++) {
                    uint16_t c = __builtin_bswap16(palette_rgb565[(uint8_t)((w >> (8 * b)) & 0xFFu)]);
                    uint16_t cd = ili9341_grid_darken(c);
                    dst32[4 * i + b] = row_dim ? ((uint32_t)cd | ((uint32_t)cd << 16))
                                               : ((uint32_t)c | ((uint32_t)cd << 16));
                }
            }
        } else {
            for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
                uint16_t map_idx = xmap[x];
                if (__builtin_expect(map_idx != 0xFFFFu, 1)) {
                    uint16_t c = palette_rgb565[indexed_lynx[map_idx] & 0xFFu];
                    if (row_dim || (x & 1u)) {
                        c = ili9341_grid_darken(c);
                    }
                    dst16[x] = __builtin_bswap16(c);
                } else {
                    dst16[x] = 0;
                }
            }
        }
    } else {
        /* Shimmerless box filter interpolation */
        for (uint16_t x = 0; x < ILI9341_WIDTH; x++) {
            uint16_t i0 = s_lynx_xweights[x].src_idx;
            if (__builtin_expect(i0 != 0xFFFFu, 1)) {
                uint8_t w1 = s_lynx_xweights[x].weight1;
                uint16_t c0 = palette_rgb565[indexed_lynx[i0] & 0xFFu];
                uint16_t c;
                if (w1 == 0 || (i0 + 1u) >= LYNX_SCREEN_W) {
                    c = c0;
                } else {
                    uint16_t c1 = palette_rgb565[indexed_lynx[i0 + 1u] & 0xFFu];
                    c = dmg_blend_rgb565(c0, c1, w1);
                }
                if (grid_on && (row_dim || (x & 1u))) {
                    c = ili9341_grid_darken(c);
                }
                dst16[x] = __builtin_bswap16(c);
            } else {
                dst16[x] = 0;
            }
        }
    }

    /* Direct FPS overlay on rows LYNX_FPS_ROW_TOP..LYNX_FPS_ROW_END-1 */
    if (s_fps_enabled && row >= LYNX_FPS_ROW_TOP && row < LYNX_FPS_ROW_END) {
        int r = (int)row - LYNX_FPS_ROW_TOP;
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

    s_slot_row[slot] = row;
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

bool __not_in_flash_func(ili9341_is_active_lynx_line)(int line) {
    if (!s_blitter_active || line < 0 || line >= LYNX_SCREEN_H) return false;
    if (!dmg_is_gb_mode() && s_scale_mode == SCALE_FIT) return true;
    if (!s_ngp_interleave) return true;
    if (s_scale_mode == SCALE_INTEGER) {
        uint8_t row = (uint8_t)(69 + line); /* (240-102)/2 = 69 */
        if (row >= ILI9341_HEIGHT) return false;
        return ((row & 1u) == s_field_parity);
    } else if (dmg_is_gb_mode() && s_scale_mode == SCALE_FIT) {
        init_lynx_stretch_tables();
        return (s_lynx_fit_gb_row_count[s_field_parity][line] > 0);
    } else {
        init_lynx_stretch_tables();
        return (s_lynx_stretch_row_count[s_field_parity][line] > 0);
    }
}

void __not_in_flash_func(ili9341_stream_lynx_line)(int line, const uint8_t *indexed_lynx, const uint16_t *palette_rgb565) {
    if (!s_blitter_active || line < 0 || line >= LYNX_SCREEN_H || indexed_lynx == NULL) return;

    if (s_scale_mode == SCALE_INTEGER) {
        /* Centered 160x102: row = 69 + line */
        uint8_t row = (uint8_t)(69 + line);
        if (row >= ILI9341_HEIGHT) return;
        if (s_ngp_interleave && ((row & 1u) != s_field_parity)) return;
        lynx_band_push(indexed_lynx, palette_rgb565, row);
    } else if (s_scale_mode == SCALE_FIT) {
        if (dmg_is_gb_mode()) {
            init_lynx_stretch_tables();
            if (s_ngp_interleave) {
                uint8_t cnt = s_lynx_fit_gb_row_count[s_field_parity][line];
                for (uint8_t i = 0; i < cnt; i++) {
                    uint8_t row = s_lynx_fit_gb_rows[s_field_parity][line][i];
                    if (row < ILI9341_HEIGHT) {
                        lynx_band_push(indexed_lynx, palette_rgb565, row);
                    }
                }
            } else {
                uint8_t cnt = s_lynx_fit_gb_prog_row_count[line];
                for (uint8_t i = 0; i < cnt; i++) {
                    uint8_t row = s_lynx_fit_gb_prog_rows[line][i];
                    if (row < ILI9341_HEIGHT) {
                        lynx_band_push(indexed_lynx, palette_rgb565, row);
                    }
                }
            }
        } else {
            /* Native aspect: uniform 2x -> 320x204 centered (18 px top/bottom), square pixels */
            uint8_t base = (uint8_t)(LYNX_FIT_TOP + line * LYNX_FIT_SCALE);
            if (s_ngp_interleave) {
                lynx_band_push(indexed_lynx, palette_rgb565, (uint8_t)(base + s_field_parity));
            } else {
                lynx_band_push(indexed_lynx, palette_rgb565, base);
                lynx_band_push(indexed_lynx, palette_rgb565, (uint8_t)(base + 1u));
            }
        }
    } else {
        /* Stretch: map 102 lines -> 240 rows vertically */
        init_lynx_stretch_tables();
        if (s_ngp_interleave) {
            uint8_t cnt = s_lynx_stretch_row_count[s_field_parity][line];
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t row = s_lynx_stretch_rows[s_field_parity][line][i];
                if (row < ILI9341_HEIGHT) {
                    lynx_band_push(indexed_lynx, palette_rgb565, row);
                }
            }
        } else {
            uint8_t cnt = s_lynx_prog_row_count[line];
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t row = s_lynx_prog_rows[line][i];
                if (row < ILI9341_HEIGHT) {
                    lynx_band_push(indexed_lynx, palette_rgb565, row);
                }
            }
        }
    }
}

void __not_in_flash_func(ili9341_stream_lynx_frame_end)(void) {
    if (!s_blitter_active) return;
    /* Low-battery overlay rows the image does not cover (Fit: y < 18, Integer:
     * y < 69): black + overlay. Hash 0 = black row, like the NULL-line path. */
    if (s_bov_frame_rows) {
        s_bov_frame_rows = false;
        const uint32_t m = dmg_batt_overlay_frame_rows();
        const uint16_t y0 = dmg_batt_overlay_y0();
        for (uint16_t i = 0; i < dmg_batt_overlay_h(); i++) {
            if (!(m & (1u << i))) continue;
            const uint16_t row = (uint16_t)(y0 + i);
            const uint8_t slot = s_write_slot;
            while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) tight_loop_contents();
            memset(s_slot_data[slot], 0, NES_ROW_BYTES);
            dmg_batt_overlay_stamp_row(row, (uint16_t *)s_slot_data[slot], 0, ILI9341_WIDTH, true);
            s_row_hash[row] = 0u;
            s_slot_row[slot] = (uint8_t)row;
            __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
            s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
        }
    }
    if (s_ngp_interleave) {
        s_field_parity ^= 1u;
    }
}

void ili9341_set_interleave(bool enable) {
    if (s_ngp_interleave != enable) {
        s_ngp_interleave = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_interleave(void) {
    return s_ngp_interleave;
}

void ili9341_set_grid_effect(bool enable) {
    if (s_grid_effect != enable) {
        s_grid_effect = enable;
        s_scanlines_enabled = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_grid_effect(void) {
    return s_grid_effect;
}

void ili9341_blitter_start(void) {
    if (s_blitter_active) return;

    /* Launch Core 1 blitter once on the first invocation */
    if (!s_blitter_core1_launched) {
        s_blitter_core1_launched = true;
        multicore_launch_core1(nes_blitter_core1);
    }

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
}

void ili9341_blitter_stop(void) {
    if (!s_blitter_active) return;

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
        s_lynx_xmap_ready = false;
        lynx_prepare_xmap();
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
        /* When not in stretch mode, explicitly clear top and bottom borders if blitter is active */
        if (s_blitter_active && s_scale_mode != SCALE_STRETCH && s_md_panel_y > 0) {
            for (int r = 0; r < s_md_panel_y; r++) {
                md_band_push(NULL, (uint8_t)r);
            }
            for (int r = s_md_panel_y + s_md_screen_h; r < ILI9341_HEIGHT; r++) {
                md_band_push(NULL, (uint8_t)r);
            }
        }
        if (s_blitter_active && s_scale_mode == SCALE_INTEGER) {
            for (int r = 0; r < 44; r++) {
                ngp_band_push(NULL, (uint8_t)r);
            }
            for (int r = 196; r < ILI9341_HEIGHT; r++) {
                ngp_band_push(NULL, (uint8_t)r);
            }
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
