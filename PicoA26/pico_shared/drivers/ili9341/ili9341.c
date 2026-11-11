#ifdef A26_BLITTER_HOST_TEST
#include "tests/blitter_stubs.h"
#else
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
#include "hardware/watchdog.h"

#include "font_8x8.h"
#endif

/* InfoNES palette table reference for backdrop color */
extern uint16_t PalTable[];

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
/* Double-buffered frame blitter state (Dual-Core PicoDMG strategy)    */
/* ------------------------------------------------------------------ */
static uint16_t s_a26_buf[2][192 * 160];
static volatile uint8_t s_write_idx = 0u;
static volatile uint8_t s_render_idx = 0u;
static volatile bool s_frame_ready = false;
static bool s_blitter_core1_launched = false;
static bool s_blitter_fault = false;
static uint32_t s_submit_time;
static bool wait_frame_bounded(void);
static volatile bool s_blitter_active = false;
static volatile bool s_scanlines_enabled = false;
static volatile ili9341_scale_mode_t s_scale_mode = SCALE_FIT;

static uint8_t s_field_parity = 0u;
static bool s_nes_interleave = false;
static uint16_t s_band_xmap[ILI9341_WIDTH];
static bool s_band_xmap_ready = false;
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
 * registers have no ST7789 equivalent and must not be sent to one. */
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
    uint32_t actual_baud=spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);
    printf("[A26] TFT SPI efetivo: %lu Hz\n",(unsigned long)actual_baud);

    nes_prepare_band_xmap();
    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("PicoA26 display: %s MADCTL=0x%02X (%s) %s (%s) @ %u Hz (Core 1 streaming)\n",
           dmg_panel_get() == DMG_PANEL_ST7789 ? "ST7789" : "ILI9341",
           (unsigned)dmg_panel_get_madctl(),
           dmg_panel_has_explicit_madctl() ? "from boot.txt" : "from panel table",
           dmg_panel_get_invert() ? "INVON" : "INVOFF",
           dmg_panel_source(),
           spi_get_baudrate(ILI9341_SPI));
}

uint16_t *ili9341_get_framebuffer(void) {
    return s_fb;
}

uint16_t *ili9341_get_draw_buffer(void) {
    return s_fb;
}

/* SPI0 ownership handshake, shared between core 0 and core 1. Declared here
 * because a26_blitter_core1 (below) serves the request. Full rationale and the
 * lock/unlock helpers are in the overlay section further down. */
static volatile bool s_spi_ovl_request = false;
static volatile bool s_spi_ovl_idle = false;
static bool s_shimmerless_enabled = true;
static dmg_scale_weight_t s_a26_gb_weights[266];
static bool s_a26_weights_ready = false;

static void a26_prepare_weights(void) {
    if (s_a26_weights_ready) return;
    dmg_init_shimmerless_weights(160, 266, s_a26_gb_weights);
    s_a26_weights_ready = true;
}

#define A26_OVL_X_FULL    8
#define A26_OVL_X_GB      53
static volatile bool s_fps_enabled;
static char s_fps_text[4];
static inline char get_font_slice(char c, int row);

/* Core 1 background display worker */

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

static void __not_in_flash_func(a26_blitter_core1)(void) {
    static uint8_t line_packed[ILI9341_WIDTH * 2u];
    uint8_t s_last_geom = 0xFFu;

    while (1) {
        if (!__atomic_load_n(&s_blitter_active,__ATOMIC_ACQUIRE)) {
            tight_loop_contents();
            continue;
        }

        /* --- SPI ownership handshake (see a26_spi_lock) ---
         * core0 asks for exclusive access to SPI0 before drawing any overlay.
         * Serve the request HERE, at the top of the loop, i.e. strictly between
         * bursts, so a request can never be granted in the middle of one. */
        if (__atomic_load_n(&s_spi_ovl_request, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s_spi_ovl_idle, true, __ATOMIC_RELEASE);
            while (__atomic_load_n(&s_spi_ovl_request, __ATOMIC_ACQUIRE)) {
                tight_loop_contents();
            }
            __atomic_store_n(&s_spi_ovl_idle, false, __ATOMIC_RELEASE);
            continue;
        }

        if (__atomic_load_n(&s_frame_ready, __ATOMIC_ACQUIRE)) {
            const uint16_t *frame_data = s_a26_buf[s_render_idx];
            const bool scanlines = s_scanlines_enabled;

            /* Stretch fills all 240 rows (192 -> 240); Fit/Integer keep 192 rows
             * centered at y=24. When the geometry changes, wipe the leftovers. */
            const bool stretch = (s_scale_mode == SCALE_STRETCH);
            const uint8_t geom = (uint8_t)((s_scale_mode & 3u) | (dmg_is_gb_mode() ? 4u : 0u));
            if (geom != s_last_geom) {
                s_last_geom = geom;
                memset(line_packed, 0, sizeof(line_packed));
                set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
                write_cmd(CMD_RAMWR);
                dc_data();
                cs_low();
                for (uint16_t cy = 0; cy < ILI9341_HEIGHT; cy++) {
                    spi_write_blocking(ILI9341_SPI, line_packed, ILI9341_WIDTH * 2u);
                }
                while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
                cs_high();
            }
            const uint16_t y_base = stretch ? 0u : 24u;
            const uint16_t n_rows = stretch ? ILI9341_HEIGHT : 192u;

            set_addr_window(0, y_base, ILI9341_WIDTH - 1, (uint16_t)(y_base + n_rows - 1u));
            write_cmd(CMD_RAMWR);

            dc_data();
            cs_low();

            for (uint16_t row = 0; row < n_rows; row++) {
                const uint16_t scr_y = (uint16_t)(y_base + row);
                const uint16_t src_y = stretch ? (uint16_t)((row * 192u) / ILI9341_HEIGHT) : row;
                const uint16_t *src160 = &frame_data[src_y * 160];
                const bool dim = scanlines && (scr_y & 1u);

                if (dmg_is_gb_mode()) {
                    if (s_scale_mode == SCALE_INTEGER) {
                        /* 160 centered in 266 (offset 45 + 53 = 98) */
                        memset(line_packed, 0, 98 * 2);
                        for (uint16_t x = 0; x < 160; x++) {
                            uint16_t c = src160[x];
                            if (dim) c = (c >> 1) & 0x7BEF;
                            line_packed[98 * 2 + 2 * x + 0] = (uint8_t)(c >> 8);
                            line_packed[98 * 2 + 2 * x + 1] = (uint8_t)(c & 0xFF);
                        }
                        memset(line_packed + (98 + 160) * 2, 0, (320 - 98 - 160) * 2);
                    } else {
                        /* Fit/Stretch: 160 mapped to 266 centered at x=45 */
                        a26_prepare_weights();
                        memset(line_packed, 0, 45 * 2);
                        if (!s_shimmerless_enabled) {
                            for (uint16_t x = 0; x < 266; x++) {
                                uint16_t sx = (uint16_t)(((uint32_t)x * 160u) / 266u);
                                uint16_t c = src160[sx];
                                if (dim) c = (c >> 1) & 0x7BEF;
                                line_packed[45 * 2 + 2 * x + 0] = (uint8_t)(c >> 8);
                                line_packed[45 * 2 + 2 * x + 1] = (uint8_t)(c & 0xFF);
                            }
                        } else {
                            for (uint16_t x = 0; x < 266; x++) {
                                uint16_t i0 = s_a26_gb_weights[x].src_idx;
                                uint8_t w1 = s_a26_gb_weights[x].weight1;
                                uint16_t c0 = src160[i0];
                                uint16_t c;
                                if (w1 == 0 || (i0 + 1u) >= 160u) {
                                    c = c0;
                                } else {
                                    uint16_t c1 = src160[i0 + 1u];
                                    c = dmg_blend_rgb565(c0, c1, w1);
                                }
                                if (dim) c = (c >> 1) & 0x7BEF;
                                line_packed[45 * 2 + 2 * x + 0] = (uint8_t)(c >> 8);
                                line_packed[45 * 2 + 2 * x + 1] = (uint8_t)(c & 0xFF);
                            }
                        }
                        memset(line_packed + (45 + 266) * 2, 0, (320 - 45 - 266) * 2);
                    }
                } else {
                    if (s_scale_mode == SCALE_INTEGER) {
                        /* 160 pixels centered in 320: 80 px left border, 80 px right border */
                        memset(line_packed, 0, 80 * 2);
                        for (uint16_t x = 0; x < 160; x++) {
                            uint16_t c = src160[x];
                            if (dim) c = (c >> 1) & 0x7BEF;
                            line_packed[160 + 2 * x + 0] = (uint8_t)(c >> 8);
                            line_packed[160 + 2 * x + 1] = (uint8_t)(c & 0xFF);
                        }
                        memset(line_packed + 240 * 2, 0, 80 * 2);
                    } else {
                        /* SCALE_FIT or SCALE_STRETCH: 2x horizontal scaling (160 -> 320) */
                        for (uint16_t x = 0; x < 160; x++) {
                            uint16_t c = src160[x];
                            if (dim) c = (c >> 1) & 0x7BEF;
                            uint8_t ch = (uint8_t)(c >> 8);
                            uint8_t cl = (uint8_t)(c & 0xFF);
                            line_packed[4 * x + 0] = ch;
                            line_packed[4 * x + 1] = cl;
                            line_packed[4 * x + 2] = ch;
                            line_packed[4 * x + 3] = cl;
                        }
                    }
                }

                /* Volume OSD HUD overlay */
                int hud_x, hud_w;
                const uint16_t *hud = dmg_volume_hud_get_row(scr_y, &hud_x, &hud_w);
                if (hud) {
                    for (int hx = 0; hx < hud_w; hx++) {
                        uint16_t c = hud[hx];
                        if (c != 0x0001) {
                            line_packed[2 * (hud_x + hx) + 0] = (uint8_t)(c >> 8);
                            line_packed[2 * (hud_x + hx) + 1] = (uint8_t)(c & 0xFF);
                        }
                    }
                }

                /* Low-battery overlay */
                dmg_batt_overlay_stamp_row(scr_y, (uint16_t *)line_packed, 0, ILI9341_WIDTH, true);

                /* FPS overlay: window rows 8..15, glyphs stamped into the line */
                if (s_fps_enabled && row >= 8u && row < 16u) {
                    const int ox = dmg_is_gb_mode() ? A26_OVL_X_GB : A26_OVL_X_FULL;
                    for (int i = 0; i < 3 && s_fps_text[i] != '\0'; i++) {
                        const uint8_t slice = (uint8_t)get_font_slice(s_fps_text[i], (int)(row - 8u));
                        for (int bit = 0; bit < 8; bit++) {
                            const int px = ox + i * 8 + bit;
                            if (px >= ILI9341_WIDTH) break;
                            const uint16_t c = (slice & (1u << bit)) ? 0xFFFFu : 0x0000u;
                            line_packed[2 * px + 0] = (uint8_t)(c >> 8);
                            line_packed[2 * px + 1] = (uint8_t)(c & 0xFF);
                        }
                    }
                }

                spi_write_blocking(ILI9341_SPI, line_packed, ILI9341_WIDTH * 2u);
            }

            while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
            while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
            cs_high();

            /* Low-battery overlay rows above the image (Fit/Integer start at y=24) */
            dmg_batt_overlay_push_borders(0, ILI9341_WIDTH, (int)y_base, (int)(y_base + n_rows), bov_push_rect);

            __atomic_store_n(&s_frame_ready, false, __ATOMIC_RELEASE);
        } else {
            tight_loop_contents();
        }
    }
}

void __not_in_flash_func(ili9341_stream_a26_line)(int line, const uint16_t *rgb565_160) {
    if (!s_blitter_active || line < 0 || line >= 192) return;
    if (rgb565_160) {
        memcpy(&s_a26_buf[s_write_idx][line * 160], rgb565_160, 160 * sizeof(uint16_t));
    } else {
        memset(&s_a26_buf[s_write_idx][line * 160], 0, 160 * sizeof(uint16_t));
    }
}

bool ili9341_try_submit_a26_frame(void) {
    if (!s_blitter_active || s_blitter_fault ||
        __atomic_load_n(&s_frame_ready,__ATOMIC_ACQUIRE)) return false;
    s_render_idx=s_write_idx;
    s_write_idx^=1u;
    s_submit_time=time_us_32();
    __atomic_store_n(&s_frame_ready,true,__ATOMIC_RELEASE);
    return true;
}
void ili9341_stream_a26_frame_end(void) { (void)ili9341_try_submit_a26_frame(); }
bool ili9341_blitter_ok(void) { return !s_blitter_fault; }
static bool wait_frame_bounded(void) {
    uint32_t started=time_us_32();
    while(__atomic_load_n(&s_frame_ready,__ATOMIC_ACQUIRE)) {
        if((uint32_t)(time_us_32()-started)>100000u) {
            s_blitter_fault=true;
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

void ili9341_stream_nes_line(int line, const uint16_t *rgb555_256) {
    (void)line;
    (void)rgb555_256;
}

void ili9341_stream_nes_frame_end(void) {
}

void ili9341_blitter_start(void) {
    if (s_blitter_active) return;

    if (!s_blitter_core1_launched) {
        s_blitter_core1_launched = true;
        multicore_launch_core1(a26_blitter_core1);
    }

    ili9341_fill_screen(0x0000);
    s_blitter_fault=false;
    s_write_idx = 0u;
    s_render_idx = 0u;
    __atomic_store_n(&s_frame_ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_blitter_active,true,__ATOMIC_RELEASE);
}

void ili9341_blitter_stop(void) {
    if(!s_blitter_active) return;
    if(s_blitter_fault || !wait_frame_bounded()) {
        // Terminate the writer before allowing menu code to touch SPI0.
        multicore_reset_core1();
        s_blitter_core1_launched=false;
        __atomic_store_n(&s_spi_ovl_request,false,__ATOMIC_RELEASE);
        __atomic_store_n(&s_spi_ovl_idle,false,__ATOMIC_RELEASE);
        spi_deinit(ILI9341_SPI);
        spi_init(ILI9341_SPI,SPI_RUN_BAUD);
        __atomic_store_n(&s_frame_ready,false,__ATOMIC_RELEASE);
        printf("[A26] Blitter timeout; core1 e SPI reiniciados\n");
    }
    __atomic_store_n(&s_blitter_active,false,__ATOMIC_RELEASE);
}

void ili9341_set_interleave(bool enable) {
    s_nes_interleave = enable;
}

/* FPS overlay (START + A), stamped scanline by scanline by the Core 1 blitter,
 * same approach as PicoGBC (mk_st7789.c). Core 0 never touches SPI0 for it, so
 * there is no bus contention and no flicker. Two digits of 8x8 glyphs on window
 * rows 8..15. */
static volatile bool s_fps_enabled = false;
static char s_fps_text[4] = "60";

void ili9341_set_fps_overlay(bool enable, const char *fps_str) {
    if (fps_str && fps_str[0] != '\0') {
        strncpy(s_fps_text, fps_str, sizeof(s_fps_text) - 1);
        s_fps_text[sizeof(s_fps_text) - 1] = '\0';
    }
    s_fps_enabled = enable;
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
    ili9341_stream_nes_frame_end();
}

void ili9341_wait_frame_done(void) {
    if(s_blitter_active && !wait_frame_bounded()) ili9341_blitter_stop();
}

bool ili9341_is_frame_busy(void) {
    bool busy=__atomic_load_n(&s_frame_ready,__ATOMIC_ACQUIRE);
    if(busy && (uint32_t)(time_us_32()-s_submit_time)>100000u) s_blitter_fault=true;
    return busy;
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
