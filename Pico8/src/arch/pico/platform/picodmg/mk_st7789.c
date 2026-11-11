/**
 * ST7789 compatibility layer that exposes the mk_ili9225_* API used by
 * main.c. This allows the emulator core to run on ST7789 SPI panels.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "mk_ili9225.h"
#include "dmg_menu.h"

/* GPIO mapping kept identical to README/main.c for LCD pins. */
#define ST7789_GPIO_CS    17u
#define ST7789_GPIO_CLK   18u
#define ST7789_GPIO_SDA   19u
#define ST7789_GPIO_DC    20u
#define ST7789_GPIO_RST   21u
#define ST7789_GPIO_LED   22u

/* Game Boy source frame dimensions. */
#define GB_LCD_WIDTH      160u
#define GB_LCD_HEIGHT     144u

/* ST7789 2.8" panels are commonly 240x320 physical.
 * We use a 320x240 logical space. Default MADCTL is rotated 180 degrees
 * from the earlier 0xA0 orientation to match the final enclosure view. */
#define ST7789_WIDTH      320u
#define ST7789_HEIGHT     240u

/* Panel-specific offsets (tweak if the image appears shifted). */
#define ST7789_X_OFFSET   0u
#define ST7789_Y_OFFSET   0u

/* Game Boy render area:
 * 160x144 scaled to 266x240 (approx 1.66x wide, 1.67x high), centered. */
#define GB_RENDER_WIDTH   266u
#define GB_RENDER_HEIGHT  240u
#define GB_RENDER_X_SHIFT 18u
#define GB_RENDER_X       (((ST7789_WIDTH - GB_RENDER_WIDTH) / 2u) + GB_RENDER_X_SHIFT)
#define GB_RENDER_Y       ((ST7789_HEIGHT - GB_RENDER_HEIGHT) / 2u)

/* ST7789 commands. */
#define ST7789_SWRESET    0x01
#define ST7789_SLPIN      0x10
#define ST7789_SLPOUT     0x11
#define ST7789_NORON      0x13
#define ST7789_INVOFF     0x20
#define ST7789_INVON      0x21
#define ST7789_DISPOFF    0x28
#define ST7789_DISPON     0x29
#define ST7789_CASET      0x2A
#define ST7789_RASET      0x2B
#define ST7789_RAMWR      0x2C
#define ST7789_MADCTL     0x36
#define ST7789_COLMOD     0x3A

#ifndef ST7789_MADCTL_VALUE
#define ST7789_MADCTL_VALUE 0x28u
#endif

#ifndef ST7789_COLMOD_VALUE
#define ST7789_COLMOD_VALUE 0x55u
#endif

#ifndef ST7789_INIT_INVERT
#define ST7789_INIT_INVERT 0
#endif

#ifndef ST7789_SWAP_RGB565_BYTES
#define ST7789_SWAP_RGB565_BYTES 0
#endif

#ifndef ST7789_RGB_ORDER
#define ST7789_RGB_ORDER 0
#endif

#ifndef ST7789_INVERT_RGB565
#define ST7789_INVERT_RGB565 0
#endif

#define ST7789_RGB_ORDER_RGB 0
#define ST7789_RGB_ORDER_RBG 1
#define ST7789_RGB_ORDER_GRB 2
#define ST7789_RGB_ORDER_GBR 3
#define ST7789_RGB_ORDER_BRG 4
#define ST7789_RGB_ORDER_BGR 5

#define ST7789_MAX_RENDER_WIDTH 320u
#define ST7789_DMA_BATCH_LINES 16u

static uint8_t s_dma_ping_pong[2][ST7789_MAX_RENDER_WIDTH * 2u * ST7789_DMA_BATCH_LINES];
static int st7789_dma_channel = -1;
static volatile bool st7789_dma_initialized = false;

static ili9341_scale_mode_t st7789_scale_mode = SCALE_FIT;
static uint16_t cur_render_width = 266u;
static uint16_t cur_render_height = 240u;
static uint16_t cur_render_x = 27u;
static uint16_t cur_render_y = 0u;
static uint8_t scale_x_map[ST7789_MAX_RENDER_WIDTH];
static bool scale_x_map_inited = false;
static uint8_t scale_y_map[ST7789_HEIGHT];
static bool scale_y_map_inited = false;
static bool st7789_grid_enabled = true;

static bool st7789_dma_queue_busy(void);
static void st7789_wait_for_idle(void);
static void st7789_write_command_raw(uint8_t cmd);
static void st7789_write_data_raw(const uint8_t *data, size_t len);
static void st7789_write_command_data_raw(uint8_t cmd, const uint8_t *data, size_t len);
static void st7789_set_addr_window_raw(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

static const char *st7789_rgb_order_name(void)
{
    switch (ST7789_RGB_ORDER) {
    case ST7789_RGB_ORDER_RGB:
        return "RGB";
    case ST7789_RGB_ORDER_RBG:
        return "RBG";
    case ST7789_RGB_ORDER_GRB:
        return "GRB";
    case ST7789_RGB_ORDER_GBR:
        return "GBR";
    case ST7789_RGB_ORDER_BRG:
        return "BRG";
    case ST7789_RGB_ORDER_BGR:
        return "BGR";
    default:
        return "RGB?";
    }
}

static inline uint16_t st7789_remap_rgb565(uint16_t c)
{
#if (ST7789_RGB_ORDER == ST7789_RGB_ORDER_RGB) && !ST7789_INVERT_RGB565
    return c;
#else
    uint8_t r5 = (uint8_t)((c >> 11) & 0x1Fu);
    uint8_t g6 = (uint8_t)((c >> 5) & 0x3Fu);
    uint8_t b5 = (uint8_t)(c & 0x1Fu);
    uint8_t out_r5 = r5;
    uint8_t out_g6 = g6;
    uint8_t out_b5 = b5;

#if ST7789_RGB_ORDER == ST7789_RGB_ORDER_RGB
    out_r5 = r5;
    out_g6 = g6;
    out_b5 = b5;
#elif ST7789_RGB_ORDER == ST7789_RGB_ORDER_RBG
    out_r5 = r5;
    out_g6 = (uint8_t)((b5 << 1) | (b5 >> 4));
    out_b5 = (uint8_t)(g6 >> 1);
#elif ST7789_RGB_ORDER == ST7789_RGB_ORDER_GRB
    out_r5 = (uint8_t)(g6 >> 1);
    out_g6 = (uint8_t)((r5 << 1) | (r5 >> 4));
    out_b5 = b5;
#elif ST7789_RGB_ORDER == ST7789_RGB_ORDER_GBR
    out_r5 = (uint8_t)(g6 >> 1);
    out_g6 = (uint8_t)((b5 << 1) | (b5 >> 4));
    out_b5 = r5;
#elif ST7789_RGB_ORDER == ST7789_RGB_ORDER_BRG
    out_r5 = b5;
    out_g6 = (uint8_t)((r5 << 1) | (r5 >> 4));
    out_b5 = (uint8_t)(g6 >> 1);
#elif ST7789_RGB_ORDER == ST7789_RGB_ORDER_BGR
    out_r5 = b5;
    out_g6 = g6;
    out_b5 = r5;
#else
#error Unsupported ST7789_RGB_ORDER value
#endif

#if ST7789_INVERT_RGB565
    out_r5 = (uint8_t)(31u - out_r5);
    out_g6 = (uint8_t)(63u - out_g6);
    out_b5 = (uint8_t)(31u - out_b5);
#endif

    return (uint16_t)(((uint16_t)out_r5 << 11) |
                      ((uint16_t)out_g6 << 5) |
                      (uint16_t)out_b5);
#endif
}

static inline void st7789_pack_rgb565(uint16_t c, uint8_t *dst)
{
    c = st7789_remap_rgb565(c);

#if ST7789_SWAP_RGB565_BYTES
    dst[0] = (uint8_t)(c & 0xFFu);
    dst[1] = (uint8_t)(c >> 8);
#else
    dst[0] = (uint8_t)(c >> 8);
    dst[1] = (uint8_t)(c & 0xFFu);
#endif
}

static inline uint16_t st7789_grid_darken(uint16_t c)
{
    /* ~14% darker: masks the pixel cell borders (LCD grid effect). */
    uint32_t r = (uint32_t)(c >> 11);
    uint32_t g = (uint32_t)((c >> 5) & 0x3Fu);
    uint32_t b = (uint32_t)(c & 0x1Fu);
    r = (r * 220u) >> 8;
    g = (g * 220u) >> 8;
    b = (b * 220u) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Populate scale_x_map[0..cur_render_width-1] with the source column index
 * for each destination column (Celeste PICO-8 source is 128x128). */
static void init_scale_x_map(void)
{
    uint16_t x;
    for (x = 0u; x < cur_render_width; ++x) {
        scale_x_map[x] = (uint8_t)(((uint32_t)x * 128u) / cur_render_width);
    }
    scale_x_map_inited = true;
}

static void init_scale_y_map(void)
{
    uint16_t y;
    for (y = 0u; y < cur_render_height; ++y) {
        scale_y_map[y] = (uint8_t)(((uint32_t)y * 128u) / cur_render_height);
    }
    scale_y_map_inited = true;
}

static void st7789_select(bool selected)
{
    gpio_put(ST7789_GPIO_CS, !selected);
}

static void st7789_dc(bool data_mode)
{
    gpio_put(ST7789_GPIO_DC, data_mode);
}

static void st7789_write_command_raw(uint8_t cmd)
{
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(true);
    st7789_dc(false);
    spi_write_blocking(spi0, &cmd, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(false);
}

static void st7789_write_data_raw(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }

    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(true);
    st7789_dc(true);
    spi_write_blocking(spi0, data, len);
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(false);
}

static void st7789_write_command_data_raw(uint8_t cmd, const uint8_t *data, size_t len)
{
    st7789_write_command_raw(cmd);
    st7789_write_data_raw(data, len);
}

static void st7789_set_addr_window_raw(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t caset[4];
    uint8_t raset[4];
    const uint8_t cmd_caset = ST7789_CASET;
    const uint8_t cmd_raset = ST7789_RASET;
    const uint8_t cmd_ramwr = ST7789_RAMWR;

    x0 += ST7789_X_OFFSET;
    x1 += ST7789_X_OFFSET;
    y0 += ST7789_Y_OFFSET;
    y1 += ST7789_Y_OFFSET;

    caset[0] = (uint8_t)(x0 >> 8);
    caset[1] = (uint8_t)(x0 & 0xFF);
    caset[2] = (uint8_t)(x1 >> 8);
    caset[3] = (uint8_t)(x1 & 0xFF);

    raset[0] = (uint8_t)(y0 >> 8);
    raset[1] = (uint8_t)(y0 & 0xFF);
    raset[2] = (uint8_t)(y1 >> 8);
    raset[3] = (uint8_t)(y1 & 0xFF);

    while (spi_is_busy(spi0)) tight_loop_contents();

    st7789_select(true);

    st7789_dc(false);
    spi_write_blocking(spi0, &cmd_caset, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();

    st7789_dc(true);
    spi_write_blocking(spi0, caset, sizeof(caset));
    while (spi_is_busy(spi0)) tight_loop_contents();

    st7789_dc(false);
    spi_write_blocking(spi0, &cmd_raset, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();

    st7789_dc(true);
    spi_write_blocking(spi0, raset, sizeof(raset));
    while (spi_is_busy(spi0)) tight_loop_contents();

    st7789_dc(false);
    spi_write_blocking(spi0, &cmd_ramwr, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();

    st7789_dc(true);
}

static bool st7789_dma_queue_busy(void)
{
    if (!st7789_dma_initialized) return false;
    return dma_channel_is_busy((uint)st7789_dma_channel) || spi_is_busy(spi0);
}

static void st7789_wait_for_idle(void)
{
    if (!st7789_dma_initialized) {
        return;
    }

    dma_channel_wait_for_finish_blocking((uint)st7789_dma_channel);

    while (spi_is_busy(spi0)) {
        tight_loop_contents();
    }

    st7789_select(false);
}

static void st7789_write_command(uint8_t cmd)
{
    st7789_wait_for_idle();
    st7789_write_command_raw(cmd);
}

static void st7789_write_data(const uint8_t *data, size_t len)
{
    st7789_wait_for_idle();
    st7789_write_data_raw(data, len);
}

static void st7789_write_command_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    st7789_wait_for_idle();
    st7789_write_command_data_raw(cmd, data, len);
}

static void st7789_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    st7789_wait_for_idle();
    st7789_set_addr_window_raw(x0, y0, x1, y1);
}

static void st7789_dma_init(void)
{
    dma_channel_config cfg;

    if (st7789_dma_initialized) {
        return;
    }

    st7789_dma_channel = dma_claim_unused_channel(true);
    cfg = dma_channel_get_default_config((uint)st7789_dma_channel);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, spi_get_dreq(spi0, true));

    dma_channel_configure((uint)st7789_dma_channel,
                          &cfg,
                          &spi_get_hw(spi0)->dr,
                          NULL,
                          0,
                          false);

    st7789_dma_initialized = true;
}

static void st7789_hw_reset(void)
{
    gpio_put(ST7789_GPIO_RST, true);
    sleep_ms(5);
    gpio_put(ST7789_GPIO_RST, false);
    sleep_ms(25);
    gpio_put(ST7789_GPIO_RST, true);
    sleep_ms(120);
}

static void st7789_init_sequence(void)
{
    const uint8_t colmod[] = {(uint8_t)ST7789_COLMOD_VALUE};
    uint8_t madctl_val = (dmg_panel_file_present() || dmg_panel_has_explicit_madctl())
                         ? dmg_panel_get_madctl()
                         : (uint8_t)ST7789_MADCTL_VALUE;
    const uint8_t madctl[] = {madctl_val};

    st7789_write_command(ST7789_SWRESET);
    sleep_ms(150);

    st7789_write_command(ST7789_SLPOUT);
    sleep_ms(120);

    bool do_invert = dmg_panel_file_present() ? dmg_panel_get_invert() : (ST7789_INIT_INVERT != 0);
    if (do_invert) {
        st7789_write_command(ST7789_INVON);
    } else {
        st7789_write_command(ST7789_INVOFF);
    }
    st7789_write_command_data(ST7789_COLMOD, colmod, sizeof(colmod));
    st7789_write_command_data(ST7789_MADCTL, madctl, sizeof(madctl));

    /* Physical panel timing: PORCTRL 0xB2 + FRCTRL2 0xC6 = 111 Hz.
     *
     * This is the block that fixed the vertical tearing on this glass, and it is
     * now the shared one in shared_menu/dmg_panel.c -- the same values the eight
     * other emulators call. st7789_write_command_data() already has the exact
     * signature dmg_panel_st7789_timing() wants, so it is passed straight in.
     *
     * The values are not "tuned": they are the ones validated here on the real
     * Core2350B, and keeping them in one place is what stops the other drivers
     * from drifting away from them again. */
    dmg_panel_st7789_timing(st7789_write_command_data);

    /* NOTE: the line below sends 0xB1 (FRMCTR1) to an ST7789. That register is
     * ILI9341-only and is not in the ST7789V command set, so it is a no-op here.
     * It was left in place deliberately: this sequence is the one that has
     * actually been running on the console, and removing a byte from a validated
     * power-on is a bigger risk than leaving a harmless one behind. */
    const uint8_t frmctr1[] = {0x00, 0x10};
    st7789_write_command_data(0xB1, frmctr1, sizeof(frmctr1));

    st7789_set_addr_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);

    st7789_write_command(ST7789_NORON);
    sleep_ms(10);
    st7789_write_command(ST7789_DISPON);
    sleep_ms(120);
}

unsigned mk_ili9225_init(void)
{
    printf("LCD: ST7789 init\n");
    printf("LCD cfg: MADCTL=0x%02X COLMOD=0x%02X INV=%u SWAP=%u ORDER=%s INV565=%u\n",
           (unsigned)((uint8_t)ST7789_MADCTL_VALUE),
           (unsigned)((uint8_t)ST7789_COLMOD_VALUE),
           (unsigned)ST7789_INIT_INVERT,
           (unsigned)ST7789_SWAP_RGB565_BYTES,
           st7789_rgb_order_name(),
           (unsigned)ST7789_INVERT_RGB565);

    /* 1. Configure GPIO pin functions and directions for SPI0 display */
    gpio_set_function(ST7789_GPIO_CS, GPIO_FUNC_SIO);
    gpio_set_function(ST7789_GPIO_CLK, GPIO_FUNC_SPI);
    gpio_set_function(ST7789_GPIO_SDA, GPIO_FUNC_SPI);
    gpio_set_function(ST7789_GPIO_DC, GPIO_FUNC_SIO);
    gpio_set_function(ST7789_GPIO_RST, GPIO_FUNC_SIO);
    gpio_set_function(ST7789_GPIO_LED, GPIO_FUNC_SIO);

    gpio_set_dir(ST7789_GPIO_CS, true);
    gpio_set_dir(ST7789_GPIO_DC, true);
    gpio_set_dir(ST7789_GPIO_RST, true);
    gpio_set_dir(ST7789_GPIO_LED, true);

    gpio_set_slew_rate(ST7789_GPIO_CLK, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(ST7789_GPIO_SDA, GPIO_SLEW_RATE_FAST);

    /* 2. Initialize SPI0 peripheral at 70 MHz (with 280 MHz sys clock = div 4) */
    spi_init(spi0, 70000000);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    /* 3. Panel reset and initialization */
    gpio_put(ST7789_GPIO_LED, false);
    st7789_select(false);
    st7789_dc(true);
    st7789_dma_init();

    st7789_hw_reset();
    st7789_init_sequence();

    mk_ili9225_set_scale_mode(st7789_scale_mode);
    mk_ili9225_fill(0x0000);
    gpio_put(ST7789_GPIO_LED, true);
    return 0;
}

void mk_ili9225_display_control(bool invert, ili9225_color_mode_e colour_mode)
{
    (void)colour_mode;
    st7789_write_command(invert ? ST7789_INVON : ST7789_INVOFF);
}

void mk_ili9225_invert_display(bool invert)
{
    mk_ili9225_display_control(invert, ILI9225_COLOR_MODE_FULL);
}

void mk_ili9225_set_window(uint16_t hor_start, uint16_t hor_end,
    uint16_t vert_start, uint16_t vert_end)
{
    if (hor_start > hor_end || vert_start > vert_end) {
        return;
    }

    if (hor_start >= ST7789_WIDTH || vert_start >= ST7789_HEIGHT) {
        return;
    }

    if (hor_end >= ST7789_WIDTH) {
        hor_end = ST7789_WIDTH - 1;
    }

    if (vert_end >= ST7789_HEIGHT) {
        vert_end = ST7789_HEIGHT - 1;
    }

    st7789_set_addr_window(hor_start, vert_start, hor_end, vert_end);
}

void mk_ili9225_set_address(uint8_t x, uint8_t y)
{
    if (x >= ST7789_WIDTH || y >= ST7789_HEIGHT) {
        return;
    }

    st7789_set_addr_window(x, y, x, y);
}

void mk_ili9225_set_x(uint8_t x)
{
    (void)x;
}

void mk_ili9225_write_pixels(const uint16_t *pixels, uint_fast16_t nmemb)
{
    (void)pixels;
    (void)nmemb;
}

void mk_ili9225_write_pixels_start(void)
{
}

void mk_ili9225_write_pixels_end(void)
{
}

/* Low-battery overlay: the part outside the rendered window (Integer, Fit),
 * which the frame never rewrites. Pixels arrive native RGB565. */
static void bov_push_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px)
{
    static uint8_t line[64 * 2];
    st7789_wait_for_idle();
    st7789_set_addr_window(x, y, (uint16_t)(x + w - 1u), (uint16_t)(y + h - 1u));
    st7789_select(true);
    st7789_dc(true);
    for (uint16_t r = 0; r < h; r++) {
        for (uint16_t i = 0; i < w && i < 64u; i++) st7789_pack_rgb565(px[r * w + i], &line[i * 2u]);
        spi_write_blocking(spi0, line, (size_t)w * 2u);
    }
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(false);
}

void mk_ili9225_draw_frame(const uint16_t *src_160x144, uint8_t fps_val)
{
    if (src_160x144 == NULL) {
        return;
    }

    if (!scale_x_map_inited) {
        init_scale_x_map();
    }
    if (!scale_y_map_inited) {
        init_scale_y_map();
    }

    st7789_wait_for_idle();

    /* Configura a janela de hardware UMA ÚNICA VEZ para o quadro inteiro */
    st7789_set_addr_window_raw(cur_render_x, cur_render_y,
                               (uint16_t)(cur_render_x + cur_render_width - 1u),
                               (uint16_t)(cur_render_y + cur_render_height - 1u));
    st7789_select(true);
    st7789_dc(true);

    uint8_t ping_pong_idx = 0;
    const size_t line_bytes = (size_t)cur_render_width * 2u;
    const bool grid_active = st7789_grid_enabled;

    char fpsString[2];
    if (fps_val > 0) {
        if (fps_val > 99) fps_val = 99;
        fpsString[0] = (char)('0' + ((fps_val / 10u) % 10u));
        fpsString[1] = (char)('0' + (fps_val % 10u));
    }

    for (uint16_t y_start = 0; y_start < cur_render_height; y_start += ST7789_DMA_BATCH_LINES) {
        uint16_t batch_lines = ST7789_DMA_BATCH_LINES;
        if ((uint16_t)(y_start + batch_lines) > cur_render_height) {
            batch_lines = (uint16_t)(cur_render_height - y_start);
        }

        uint8_t *batch_buf = s_dma_ping_pong[ping_pong_idx];

        for (uint16_t b = 0; b < batch_lines; ++b) {
            uint16_t y = (uint16_t)(y_start + b);
            uint8_t *dst = &batch_buf[(size_t)b * line_bytes];
            uint8_t src_gb_y = scale_y_map[y];
            const uint16_t *src_row = &src_160x144[(size_t)src_gb_y * GB_LCD_WIDTH];

            if (grid_active) {
                const bool y_dark = ((y & 1u) == 1u);
                if (y_dark) {
                    for (uint16_t x = 0; x < cur_render_width; ++x) {
                        uint16_t c = src_row[scale_x_map[x]];
                        st7789_pack_rgb565(st7789_grid_darken(c), &dst[x * 2u]);
                    }
                } else {
                    for (uint16_t x = 0; x < cur_render_width; ++x) {
                        uint16_t c = src_row[scale_x_map[x]];
                        if (x & 1u) {
                            c = st7789_grid_darken(c);
                        }
                        st7789_pack_rgb565(c, &dst[x * 2u]);
                    }
                }
            } else {
                for (uint16_t x = 0; x < cur_render_width; ++x) {
                    uint16_t c = src_row[scale_x_map[x]];
                    st7789_pack_rgb565(c, &dst[x * 2u]);
                }
            }

            /* Volume OSD HUD scanline stamp */
            uint16_t screen_y = (uint16_t)(cur_render_y + y);
            int hud_x, hud_w;
            const uint16_t *hud = dmg_volume_hud_get_row(screen_y, &hud_x, &hud_w);
            if (hud) {
                int render_x0 = (int)cur_render_x;
                int render_w = (int)cur_render_width;
                for (int i = 0; i < hud_w; i++) {
                    int local_x = (hud_x + i) - render_x0;
                    if (local_x >= 0 && local_x < render_w) {
                        uint16_t c = hud[i];
                        if (c != 0x0001) {
                            st7789_pack_rgb565(c, &dst[local_x * 2u]);
                        }
                    }
                }
            }

            /* Low-battery overlay */
            dmg_batt_overlay_stamp_row(screen_y, (uint16_t *)dst, (int)cur_render_x, (int)cur_render_width, true);

            /* FPS overlay (scanlines 8 a 15) */
            if (fps_val > 0 && y >= 8 && y < 16) {
                int rowInChar = (int)(y - 8);
                int x_start = 8;
                for (int i = 0; i < 2; i++) {
                    uint8_t glyph[8];
                    mk_ili9225_get_raw_glyph(glyph, fpsString[i]);
                    uint8_t fontSlice = glyph[rowInChar];
                    for (int bit = 0; bit < 8; bit++) {
                        int px = x_start + i * 8 + bit;
                        if (px < (int)cur_render_width) {
                            uint16_t c = (fontSlice & (0x80 >> bit)) ? 0xFFFF : 0x0000;
                            st7789_pack_rgb565(c, &dst[px * 2u]);
                        }
                    }
                }
            }
        }

        /* Espera o batch anterior do DMA terminar antes de despachar o novo */
        dma_channel_wait_for_finish_blocking((uint)st7789_dma_channel);
        dma_channel_transfer_from_buffer_now((uint)st7789_dma_channel, batch_buf, line_bytes * batch_lines);

        ping_pong_idx ^= 1u;
    }

    /* Aguarda o último batch de DMA finalizar completamente */
    dma_channel_wait_for_finish_blocking((uint)st7789_dma_channel);
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(false);

    dmg_batt_overlay_push_borders((int)cur_render_x, (int)(cur_render_x + cur_render_width),
                                  (int)cur_render_y, (int)(cur_render_y + cur_render_height), bov_push_rect);

    /* Volume HUD rodapé em modo Integer (linhas 192..199) */
    if (cur_render_height < ST7789_HEIGHT && dmg_volume_hud_is_active()) {
        int hx, hw;
        const uint16_t *row192 = dmg_volume_hud_get_row(192, &hx, &hw);
        if (row192 && hw > 0) {
            static uint8_t s_hud_bot[8 * 160 * 2];
            for (int r = 0; r < 8; r++) {
                const uint16_t *hrow = dmg_volume_hud_get_row((uint16_t)(192 + r), NULL, NULL);
                if (!hrow) break;
                for (int c = 0; c < hw; c++) {
                    st7789_pack_rgb565(hrow[c], &s_hud_bot[(r * hw + c) * 2]);
                }
            }
            st7789_wait_for_idle();
            st7789_set_addr_window((uint16_t)hx, 192, (uint16_t)(hx + hw - 1), 199);
            st7789_select(true);
            st7789_dc(true);
            spi_write_blocking(spi0, s_hud_bot, (size_t)hw * 8u * 2u);
            while (spi_is_busy(spi0)) tight_loop_contents();
            st7789_select(false);
        }
    }
}

void mk_st7789_draw_celeste_frame(const uint8_t *src_128x128, const uint16_t *pal_16, uint8_t fps_val)
{
    if (src_128x128 == NULL || pal_16 == NULL) {
        return;
    }

    if (!scale_x_map_inited) {
        init_scale_x_map();
    }
    if (!scale_y_map_inited) {
        init_scale_y_map();
    }

    st7789_wait_for_idle();

    st7789_set_addr_window_raw(cur_render_x, cur_render_y,
                               (uint16_t)(cur_render_x + cur_render_width - 1u),
                               (uint16_t)(cur_render_y + cur_render_height - 1u));
    st7789_select(true);
    st7789_dc(true);

    uint8_t ping_pong_idx = 0;
    const size_t line_bytes = (size_t)cur_render_width * 2u;
    const bool grid_active = st7789_grid_enabled;

    char fpsString[2];
    if (fps_val > 0) {
        if (fps_val > 99) fps_val = 99;
        fpsString[0] = (char)('0' + ((fps_val / 10u) % 10u));
        fpsString[1] = (char)('0' + (fps_val % 10u));
    }

    for (uint16_t y_start = 0; y_start < cur_render_height; y_start += ST7789_DMA_BATCH_LINES) {
        uint16_t batch_lines = ST7789_DMA_BATCH_LINES;
        if ((uint16_t)(y_start + batch_lines) > cur_render_height) {
            batch_lines = (uint16_t)(cur_render_height - y_start);
        }

        uint8_t *batch_buf = s_dma_ping_pong[ping_pong_idx];

        for (uint16_t b = 0; b < batch_lines; ++b) {
            uint16_t y = (uint16_t)(y_start + b);
            uint8_t *dst = &batch_buf[(size_t)b * line_bytes];
            uint8_t src_y = scale_y_map[y];
            const uint8_t *src_row = &src_128x128[(size_t)src_y * 128u];

            if (grid_active) {
                const bool y_dark = ((y & 1u) == 1u);
                if (y_dark) {
                    for (uint16_t x = 0; x < cur_render_width; ++x) {
                        uint16_t c = pal_16[src_row[scale_x_map[x]] & 0x0Fu];
                        st7789_pack_rgb565(st7789_grid_darken(c), &dst[x * 2u]);
                    }
                } else {
                    for (uint16_t x = 0; x < cur_render_width; ++x) {
                        uint16_t c = pal_16[src_row[scale_x_map[x]] & 0x0Fu];
                        if (x & 1u) {
                            c = st7789_grid_darken(c);
                        }
                        st7789_pack_rgb565(c, &dst[x * 2u]);
                    }
                }
            } else {
                for (uint16_t x = 0; x < cur_render_width; ++x) {
                    uint16_t c = pal_16[src_row[scale_x_map[x]] & 0x0Fu];
                    st7789_pack_rgb565(c, &dst[x * 2u]);
                }
            }

            /* Volume OSD HUD scanline stamp */
            uint16_t screen_y = (uint16_t)(cur_render_y + y);
            int hud_x, hud_w;
            const uint16_t *hud = dmg_volume_hud_get_row(screen_y, &hud_x, &hud_w);
            if (hud) {
                int render_x0 = (int)cur_render_x;
                int render_w = (int)cur_render_width;
                for (int i = 0; i < hud_w; i++) {
                    int local_x = (hud_x + i) - render_x0;
                    if (local_x >= 0 && local_x < render_w) {
                        uint16_t c = hud[i];
                        if (c != 0x0001) {
                            st7789_pack_rgb565(c, &dst[local_x * 2u]);
                        }
                    }
                }
            }

            /* Low-battery overlay */
            dmg_batt_overlay_stamp_row(screen_y, (uint16_t *)dst, (int)cur_render_x, (int)cur_render_width, true);

            /* FPS overlay (scanlines 8 a 15) */
            if (fps_val > 0 && y >= 8 && y < 16) {
                int rowInChar = (int)(y - 8);
                int x_start = 8;
                for (int i = 0; i < 2; i++) {
                    uint8_t glyph[8];
                    mk_ili9225_get_raw_glyph(glyph, fpsString[i]);
                    uint8_t fontSlice = glyph[rowInChar];
                    for (int bit = 0; bit < 8; bit++) {
                        int px = x_start + i * 8 + bit;
                        if (px < (int)cur_render_width) {
                            uint16_t c = (fontSlice & (0x80 >> bit)) ? 0xFFFF : 0x0000;
                            st7789_pack_rgb565(c, &dst[px * 2u]);
                        }
                    }
                }
            }
        }

        dma_channel_wait_for_finish_blocking((uint)st7789_dma_channel);
        dma_channel_transfer_from_buffer_now((uint)st7789_dma_channel, batch_buf, line_bytes * batch_lines);

        ping_pong_idx ^= 1u;
    }

    dma_channel_wait_for_finish_blocking((uint)st7789_dma_channel);
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(false);

    dmg_batt_overlay_push_borders((int)cur_render_x, (int)(cur_render_x + cur_render_width),
                                  (int)cur_render_y, (int)(cur_render_y + cur_render_height), bov_push_rect);

    if (cur_render_height < ST7789_HEIGHT && dmg_volume_hud_is_active()) {
        int hx, hw;
        const uint16_t *row192 = dmg_volume_hud_get_row(192, &hx, &hw);
        if (row192 && hw > 0) {
            static uint8_t s_hud_bot[8 * 160 * 2];
            for (int r = 0; r < 8; r++) {
                const uint16_t *hrow = dmg_volume_hud_get_row((uint16_t)(192 + r), NULL, NULL);
                if (!hrow) break;
                for (int c = 0; c < hw; c++) {
                    st7789_pack_rgb565(hrow[c], &s_hud_bot[(r * hw + c) * 2]);
                }
            }
            st7789_wait_for_idle();
            st7789_set_addr_window((uint16_t)hx, 192, (uint16_t)(hx + hw - 1), 199);
            st7789_select(true);
            st7789_dc(true);
            spi_write_blocking(spi0, s_hud_bot, (size_t)hw * 8u * 2u);
            while (spi_is_busy(spi0)) tight_loop_contents();
            st7789_select(false);
        }
    }
}

void __time_critical_func(mk_st7789_draw_pico8_frame)(const uint8_t *fb_4bit_128x128, const uint16_t *pal_16, uint8_t fps_val)
{
    if (fb_4bit_128x128 == NULL || pal_16 == NULL) {
        return;
    }

    if (!scale_x_map_inited) {
        init_scale_x_map();
    }
    if (!scale_y_map_inited) {
        init_scale_y_map();
    }

    st7789_wait_for_idle();

    st7789_set_addr_window_raw(cur_render_x, cur_render_y,
                               (uint16_t)(cur_render_x + cur_render_width - 1u),
                               (uint16_t)(cur_render_y + cur_render_height - 1u));
    st7789_select(true);
    st7789_dc(true);

    uint8_t ping_pong_idx = 0;
    const size_t line_bytes = (size_t)cur_render_width * 2u;
    const bool grid_active = st7789_grid_enabled;

    /* Precompute packed palette entries (with byte swapping for SPI DMA)
     * eliminates 57,600 function calls and remappings per frame! */
    uint16_t pal_normal_packed[16];
    uint16_t pal_dark_packed[16];
    for (int i = 0; i < 16; i++) {
        uint16_t c = pal_16[i];
        uint16_t remap = st7789_remap_rgb565(c);
        pal_normal_packed[i] = (uint16_t)((remap >> 8) | (remap << 8));
        uint16_t dark = st7789_remap_rgb565(st7789_grid_darken(c));
        pal_dark_packed[i] = (uint16_t)((dark >> 8) | (dark << 8));
    }

    char fpsString[2];
    if (fps_val > 0) {
        if (fps_val > 99) fps_val = 99;
        fpsString[0] = (char)('0' + ((fps_val / 10u) % 10u));
        fpsString[1] = (char)('0' + (fps_val % 10u));
    }

    for (uint16_t y_start = 0; y_start < cur_render_height; y_start += ST7789_DMA_BATCH_LINES) {
        uint16_t batch_lines = ST7789_DMA_BATCH_LINES;
        if ((uint16_t)(y_start + batch_lines) > cur_render_height) {
            batch_lines = (uint16_t)(cur_render_height - y_start);
        }

        uint8_t *batch_buf = s_dma_ping_pong[ping_pong_idx];

        for (uint16_t b = 0; b < batch_lines; ++b) {
            uint16_t y = (uint16_t)(y_start + b);
            uint8_t *dst = &batch_buf[(size_t)b * line_bytes];
            uint16_t *dst16 = (uint16_t*)dst;
            uint8_t src_y = scale_y_map[y];
            const uint8_t *src_row = &fb_4bit_128x128[(size_t)src_y * 64u];

            if (grid_active) {
                const bool y_dark = ((y & 1u) == 1u);
                if (y_dark) {
                    for (uint16_t x = 0; x < cur_render_width; ++x) {
                        uint8_t sx = scale_x_map[x];
                        uint8_t byte_val = src_row[sx >> 1];
                        uint8_t c_idx = (sx & 1u) ? (byte_val >> 4) : (byte_val & 0x0Fu);
                        dst16[x] = pal_dark_packed[c_idx & 0x0Fu];
                    }
                } else {
                    for (uint16_t x = 0; x < cur_render_width; ++x) {
                        uint8_t sx = scale_x_map[x];
                        uint8_t byte_val = src_row[sx >> 1];
                        uint8_t c_idx = (sx & 1u) ? (byte_val >> 4) : (byte_val & 0x0Fu);
                        dst16[x] = (x & 1u) ? pal_dark_packed[c_idx & 0x0Fu] : pal_normal_packed[c_idx & 0x0Fu];
                    }
                }
            } else {
                for (uint16_t x = 0; x < cur_render_width; ++x) {
                    uint8_t sx = scale_x_map[x];
                    uint8_t byte_val = src_row[sx >> 1];
                    uint8_t c_idx = (sx & 1u) ? (byte_val >> 4) : (byte_val & 0x0Fu);
                    dst16[x] = pal_normal_packed[c_idx & 0x0Fu];
                }
            }

            /* Volume OSD HUD scanline stamp */
            uint16_t screen_y = (uint16_t)(cur_render_y + y);
            int hud_x, hud_w;
            const uint16_t *hud = dmg_volume_hud_get_row(screen_y, &hud_x, &hud_w);
            if (hud) {
                int render_x0 = (int)cur_render_x;
                int render_w = (int)cur_render_width;
                for (int i = 0; i < hud_w; i++) {
                    int local_x = (hud_x + i) - render_x0;
                    if (local_x >= 0 && local_x < render_w) {
                        uint16_t c = hud[i];
                        if (c != 0x0001) {
                            st7789_pack_rgb565(c, &dst[local_x * 2u]);
                        }
                    }
                }
            }

            /* Low-battery overlay */
            dmg_batt_overlay_stamp_row(screen_y, (uint16_t *)dst, (int)cur_render_x, (int)cur_render_width, true);

            if (fps_val > 0 && y >= 8 && y < 16) {
                int rowInChar = y - 8;
                int x_start = (int)cur_render_width - 18;
                if (x_start < 0) x_start = 0;
                for (int i = 0; i < 2; i++) {
                    uint8_t glyph[8];
                    mk_ili9225_get_raw_glyph(glyph, fpsString[i]);
                    uint8_t fontSlice = glyph[rowInChar];
                    for (int bit = 0; bit < 8; bit++) {
                        int px = x_start + i * 8 + bit;
                        if (px < (int)cur_render_width) {
                            uint16_t c = (fontSlice & (0x80 >> bit)) ? 0xFFFF : 0x0000;
                            st7789_pack_rgb565(c, &dst[px * 2u]);
                        }
                    }
                }
            }
        }

        dma_channel_wait_for_finish_blocking((uint)st7789_dma_channel);
        dma_channel_transfer_from_buffer_now((uint)st7789_dma_channel, batch_buf, line_bytes * batch_lines);
        ping_pong_idx ^= 1u;
    }

    dma_channel_wait_for_finish_blocking((uint)st7789_dma_channel);
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(false);

    dmg_batt_overlay_push_borders((int)cur_render_x, (int)(cur_render_x + cur_render_width),
                                  (int)cur_render_y, (int)(cur_render_y + cur_render_height), bov_push_rect);

    static bool s_last_hud_active_int = false;
    if (cur_render_height < ST7789_HEIGHT) {
        if (dmg_volume_hud_is_active()) {
            s_last_hud_active_int = true;
            int hx, hw;
            const uint16_t *row184 = dmg_volume_hud_get_row(184, &hx, &hw);
            if (row184 && hw > 0) {
                static uint8_t s_hud_bot[16 * 160 * 2];
                for (int r = 0; r < 16; r++) {
                    const uint16_t *hrow = dmg_volume_hud_get_row((uint16_t)(184 + r), NULL, NULL);
                    if (!hrow) break;
                    for (int c = 0; c < hw; c++) {
                        st7789_pack_rgb565(hrow[c], &s_hud_bot[(r * hw + c) * 2]);
                    }
                }
                st7789_wait_for_idle();
                st7789_set_addr_window((uint16_t)hx, 184, (uint16_t)(hx + hw - 1), 199);
                st7789_select(true);
                st7789_dc(true);
                spi_write_blocking(spi0, s_hud_bot, (size_t)hw * 16u * 2u);
                while (spi_is_busy(spi0)) tight_loop_contents();
                st7789_select(false);
            }
        } else if (s_last_hud_active_int) {
            s_last_hud_active_int = false;
            bool gb = dmg_is_gb_mode();
            uint16_t hx = gb ? 111 : 80;
            uint16_t hw = gb ? 134 : 160;
            static uint8_t s_black_bot[16 * 160 * 2] = {0};
            st7789_wait_for_idle();
            st7789_set_addr_window(hx, 184, (uint16_t)(hx + hw - 1), 199);
            st7789_select(true);
            st7789_dc(true);
            spi_write_blocking(spi0, s_black_bot, (size_t)hw * 16u * 2u);
            while (spi_is_busy(spi0)) tight_loop_contents();
            st7789_select(false);
        }
    }
}

void mk_st7789_set_brightness(uint8_t level) {
    if (level > 5) level = 5;
    gpio_init(ST7789_GPIO_LED);
    gpio_set_dir(ST7789_GPIO_LED, GPIO_OUT);
    gpio_put(ST7789_GPIO_LED, level > 0);
}

void mk_ili9225_power_control(uint8_t drive_power, bool sleep)
{
    (void)drive_power;
    st7789_write_command(sleep ? ST7789_SLPIN : ST7789_SLPOUT);
}

void mk_ili9225_set_grid_enabled(bool enabled)
{
    st7789_grid_enabled = enabled;
}

bool mk_ili9225_get_grid_enabled(void)
{
    return st7789_grid_enabled;
}

void mk_ili9225_set_gate_scan(uint16_t hor_start, uint16_t hor_end)
{
    (void)hor_start;
    (void)hor_end;
}

void mk_ili9225_set_drive_freq(uint16_t f)
{
    (void)f;
}

void mk_ili9225_exit(void)
{
    st7789_write_command(ST7789_DISPOFF);
}

void mk_ili9225_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    static uint8_t color_block[128];
    uint16_t x0 = x;
    uint16_t y0 = y;
    uint16_t x1;
    uint16_t y1;
    uint32_t pixels;
    size_t i;

    if (w == 0 || h == 0) {
        return;
    }

    if (x0 >= ST7789_WIDTH || y0 >= ST7789_HEIGHT) {
        return;
    }

    x1 = (uint16_t)(x0 + w - 1);
    y1 = (uint16_t)(y0 + h - 1);

    if (x1 >= ST7789_WIDTH) {
        x1 = ST7789_WIDTH - 1;
    }

    if (y1 >= ST7789_HEIGHT) {
        y1 = ST7789_HEIGHT - 1;
    }

    st7789_pack_rgb565(color, &color_block[0]);
    for (i = 2; i < sizeof(color_block); i += 2) {
        color_block[i] = color_block[0];
        color_block[i + 1] = color_block[1];
    }

    st7789_set_addr_window(x0, y0, x1, y1);

    st7789_select(true);
    st7789_dc(true);
    pixels = ((uint32_t)x1 - x0 + 1u) * ((uint32_t)y1 - y0 + 1u);
    while (pixels != 0u) {
        uint32_t chunk_pixels = (uint32_t)(sizeof(color_block) / 2u);
        if (chunk_pixels > pixels) {
            chunk_pixels = pixels;
        }
        spi_write_blocking(spi0, color_block, chunk_pixels * 2u);
        pixels -= chunk_pixels;
    }
    st7789_select(false);
}

/* Direct-to-panel block write, host-order RGB565. Used by the standby-resume
 * intro (shared_menu/dmg_boot_intro.c), which runs on core 1 while core 0 loads
 * the cart -- so it only touches the SPI, never the frame buffers. */
void mk_ili9225_write_rect_565(int x, int y, int w, int h, const uint16_t *px)
{
    uint8_t buf[64];
    size_t n = 0;

    if (px == NULL || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > (int)ST7789_WIDTH || y + h > (int)ST7789_HEIGHT) return;

    st7789_set_addr_window((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    st7789_select(true);
    st7789_dc(true);
    for (int i = 0; i < w * h; i++) {
        st7789_pack_rgb565(px[i], &buf[n]);
        n += 2u;
        if (n == sizeof(buf)) {
            spi_write_blocking(spi0, buf, n);
            n = 0;
        }
    }
    if (n) spi_write_blocking(spi0, buf, n);
    while (spi_is_busy(spi0)) tight_loop_contents();
    st7789_select(false);
}

void mk_ili9225_fill(uint16_t color)
{
    static uint8_t color_block[128];
    uint32_t pixels;
    size_t i;

    st7789_pack_rgb565(color, &color_block[0]);
    for (i = 2; i < sizeof(color_block); i += 2) {
        color_block[i] = color_block[0];
        color_block[i + 1] = color_block[1];
    }

    st7789_set_addr_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);
    st7789_select(true);
    st7789_dc(true);

    pixels = (uint32_t)ST7789_WIDTH * (uint32_t)ST7789_HEIGHT;
    while (pixels != 0u) {
        uint32_t chunk_pixels = (uint32_t)(sizeof(color_block) / 2u);
        if (chunk_pixels > pixels) {
            chunk_pixels = pixels;
        }
        spi_write_blocking(spi0, color_block, chunk_pixels * 2u);
        pixels -= chunk_pixels;
    }

    st7789_select(false);
}

void mk_ili9225_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    mk_ili9225_fill_rect(x, y, 1, 1, color);
}

void mk_ili9225_blit(uint16_t *fbuf, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    static uint8_t txbuf[8 * 8 * 2];
    uint16_t x0 = x;
    uint16_t y0 = y;
    uint16_t x1;
    uint16_t y1;
    uint32_t pixels;
    uint32_t sent = 0;

    if (fbuf == NULL || w == 0 || h == 0) {
        return;
    }

    if (x0 >= ST7789_WIDTH || y0 >= ST7789_HEIGHT) {
        return;
    }

    x1 = (uint16_t)(x0 + w - 1);
    y1 = (uint16_t)(y0 + h - 1);

    if (x1 >= ST7789_WIDTH) {
        x1 = ST7789_WIDTH - 1;
    }

    if (y1 >= ST7789_HEIGHT) {
        y1 = ST7789_HEIGHT - 1;
    }

    pixels = ((uint32_t)x1 - x0 + 1u) * ((uint32_t)y1 - y0 + 1u);
    st7789_set_addr_window(x0, y0, x1, y1);

    st7789_select(true);
    st7789_dc(true);
    while (sent < pixels) {
        uint32_t chunk_pixels = (uint32_t)(sizeof(txbuf) / 2u);
        uint32_t i;

        if (chunk_pixels > (pixels - sent)) {
            chunk_pixels = pixels - sent;
        }

        for (i = 0; i < chunk_pixels; ++i) {
            uint16_t c = fbuf[sent + i];
            st7789_pack_rgb565(c, &txbuf[i * 2u]);
        }

        spi_write_blocking(spi0, txbuf, chunk_pixels * 2u);
        sent += chunk_pixels;
    }
    st7789_select(false);
}

/* Large-window raw blit (16-bit coordinates / dimensions). Every pixel goes
 * through st7789_pack_rgb565 so the RGB order, byte swap and invert settings
 * stay consistent with the rest of the driver. */
void mk_ili9225_blit_window(const uint16_t *fbuf, uint16_t x0, uint16_t y0,
                            uint16_t w, uint16_t h)
{
    static uint8_t txbuf[266 * 2];
    uint16_t x1;
    uint16_t y1;
    uint16_t clip_w;
    uint16_t draw_w;
    uint16_t draw_h;

    if (fbuf == NULL || w == 0 || h == 0) {
        return;
    }

    if (x0 >= ST7789_WIDTH || y0 >= ST7789_HEIGHT) {
        return;
    }

    x1 = (uint16_t)(x0 + w - 1);
    y1 = (uint16_t)(y0 + h - 1);

    if (x1 >= ST7789_WIDTH) {
        x1 = ST7789_WIDTH - 1;
    }

    if (y1 >= ST7789_HEIGHT) {
        y1 = ST7789_HEIGHT - 1;
    }

    clip_w = (uint16_t)(x1 - x0 + 1u);
    draw_w = clip_w;
    draw_h = (uint16_t)(y1 - y0 + 1u);

    if (clip_w > w) {
        clip_w = w;
    }

    if (clip_w > (uint16_t)(sizeof(txbuf) / 2u)) {
        clip_w = (uint16_t)(sizeof(txbuf) / 2u);
    }

    st7789_set_addr_window(x0, y0, x1, y1);

    st7789_select(true);
    st7789_dc(true);
    for (uint16_t row = 0; row < draw_h; ++row) {
        if (row >= h) {
            continue;
        }
        const uint16_t *src_row = &fbuf[(size_t)row * w];
        for (uint16_t i = 0; i < clip_w; ++i) {
            st7789_pack_rgb565(src_row[i], &txbuf[i * 2u]);
        }
        spi_write_blocking(spi0, txbuf, clip_w * 2u);
    }
    st7789_select(false);
}

/* Fast whole-frame blit for the NES 256x240 framebuffer: set the address
 * window once and stream the frame with 16-bit blocking SPI writes. Kept
 * CPU-driven (no DMA channel) on purpose: the shared DMA_IRQ_1 handler and
 * channel state are owned by the GB band queue, and claiming a second DMA
 * channel here deadlocked game startup. At 60 MHz SPI this is one packed
 * 16-bit write per pixel with zero per-row window re-setup. */
void mk_ili9225_blit_window_fast(const uint16_t *fbuf, uint16_t x0,
                                 uint16_t y0, uint16_t w, uint16_t h)
{
    uint16_t x1;
    uint16_t y1;
    uint16_t draw_w;
    uint16_t draw_h;

    if (fbuf == NULL || w == 0 || h == 0) {
        return;
    }
    if (x0 >= ST7789_WIDTH || y0 >= ST7789_HEIGHT) {
        return;
    }
    x1 = (uint16_t)(x0 + w - 1u);
    y1 = (uint16_t)(y0 + h - 1u);
    if (x1 >= ST7789_WIDTH) {
        x1 = ST7789_WIDTH - 1u;
    }
    if (y1 >= ST7789_HEIGHT) {
        y1 = ST7789_HEIGHT - 1u;
    }
    draw_w = (uint16_t)(x1 - x0 + 1u);
    draw_h = (uint16_t)(y1 - y0 + 1u);

    st7789_set_addr_window(x0, y0, x1, y1);
    st7789_select(true);
    st7789_dc(true);

    /* Single address window, then one 16-bit blocking write per pixel. The
     * source is RGB565, tightly packed, so no byte swap unless configured. */
    if (draw_w == w) {
        const size_t npix = (size_t)draw_w * draw_h;
        for (size_t i = 0; i < npix; ++i) {
            uint16_t c = st7789_remap_rgb565(fbuf[i]);
#if ST7789_SWAP_RGB565_BYTES
            c = (uint16_t)((c >> 8) | (c << 8));
#endif
            spi_write16_blocking(spi0, &c, 1);
        }
    } else {
        for (uint16_t row = 0; row < draw_h; ++row) {
            const uint16_t *src_row = &fbuf[(size_t)row * w];
            for (uint16_t i = 0; i < draw_w; ++i) {
                uint16_t c = st7789_remap_rgb565(src_row[i]);
#if ST7789_SWAP_RGB565_BYTES
                c = (uint16_t)((c >> 8) | (c << 8));
#endif
                spi_write16_blocking(spi0, &c, 1);
            }
        }
    }
    st7789_select(false);
}

/* NES interlaced row streaming.
 *
 * The bezel is 266x240 = 127680 bytes per full frame. SPI0 runs at
 * clk_peri/4 = 31.25 MHz (the PL022 divider cannot hit the requested
 * 60 MHz from a 125 MHz clk_peri), so a full frame costs ~32.7 ms - about
 * twice the 16.64 ms NTSC frame budget, which capped the emulator near
 * 50-55% speed. Sending one field (every other panel row) per frame halves
 * that to ~16.3 ms and fits, exactly like the GB app's interlace mode.
 *
 * Each row is addressed on its own: CASET is programmed once per frame and
 * only RASET+RAMWR are re-issued per row. RAMWR resets the panel's address
 * counter to the window origin, so a row can never land on the wrong line
 * even if a frame is cut short (that ordering dependency was what produced
 * the tearing/right-edge garbage in the earlier streaming attempt).
 * SPI stays 8-bit and blocking - no DMA on this path. */
void mk_ili9225_nes_frame_begin(void)
{
    uint8_t caset[4];
    const uint16_t x0 = (uint16_t)(GB_RENDER_X + ST7789_X_OFFSET);
    const uint16_t x1 = (uint16_t)(x0 + GB_RENDER_WIDTH - 1u);

    st7789_wait_for_idle();

    caset[0] = (uint8_t)(x0 >> 8);
    caset[1] = (uint8_t)(x0 & 0xFFu);
    caset[2] = (uint8_t)(x1 >> 8);
    caset[3] = (uint8_t)(x1 & 0xFFu);
    st7789_write_command_data_raw(ST7789_CASET, caset, sizeof(caset));
}

void mk_ili9225_stream_nes_row(const uint8_t *packed, uint16_t row)
{
    uint8_t caset[4];
    uint8_t raset[4];
    uint16_t x0, x1, y;

    if (packed == NULL || row >= GB_RENDER_HEIGHT) {
        return;
    }

    x0 = (uint16_t)(GB_RENDER_X + ST7789_X_OFFSET);
    x1 = (uint16_t)(x0 + GB_RENDER_WIDTH - 1u);
    caset[0] = (uint8_t)(x0 >> 8);
    caset[1] = (uint8_t)(x0 & 0xFFu);
    caset[2] = (uint8_t)(x1 >> 8);
    caset[3] = (uint8_t)(x1 & 0xFFu);
    st7789_write_command_data_raw(ST7789_CASET, caset, sizeof(caset));

    y = (uint16_t)(GB_RENDER_Y + ST7789_Y_OFFSET + row);
    raset[0] = (uint8_t)(y >> 8);
    raset[1] = (uint8_t)(y & 0xFFu);
    raset[2] = (uint8_t)(y >> 8);
    raset[3] = (uint8_t)(y & 0xFFu);
    st7789_write_command_data_raw(ST7789_RASET, raset, sizeof(raset));
    st7789_write_command_raw(ST7789_RAMWR);

    st7789_select(true);
    st7789_dc(true);
    spi_write_blocking(spi0, packed, GB_RENDER_WIDTH * 2u);
    st7789_select(false);
}

void mk_ili9225_stream_nes_row_win(const uint8_t *packed, uint16_t row,
                                   uint16_t x0, uint16_t w)
{
    uint8_t caset[4];
    uint8_t raset[4];
    uint16_t xb, xe, y;

    if (packed == NULL || row >= GB_RENDER_HEIGHT || w == 0u ||
        x0 + w > GB_RENDER_WIDTH) {
        return;
    }

    xb = (uint16_t)(GB_RENDER_X + ST7789_X_OFFSET + x0);
    xe = (uint16_t)(xb + w - 1u);
    caset[0] = (uint8_t)(xb >> 8);
    caset[1] = (uint8_t)(xb & 0xFFu);
    caset[2] = (uint8_t)(xe >> 8);
    caset[3] = (uint8_t)(xe & 0xFFu);
    st7789_write_command_data_raw(ST7789_CASET, caset, sizeof(caset));

    y = (uint16_t)(GB_RENDER_Y + ST7789_Y_OFFSET + row);
    raset[0] = (uint8_t)(y >> 8);
    raset[1] = (uint8_t)(y & 0xFFu);
    raset[2] = (uint8_t)(y >> 8);
    raset[3] = (uint8_t)(y & 0xFFu);
    st7789_write_command_data_raw(ST7789_RASET, raset, sizeof(raset));
    st7789_write_command_raw(ST7789_RAMWR);

    st7789_select(true);
    st7789_dc(true);
    spi_write_blocking(spi0, packed, (size_t)w * 2u);
    st7789_select(false);
}

void mk_ili9225_nes_frame_end(void)
{
    /* Every row deasserts CS on its own; nothing is left in flight. */
}

void mk_ili9225_get_raw_glyph(uint8_t d[8], char l) {
	uint8_t *letter = d;
	memset(d, 0, 8);
	
	switch(l)
	{
		case 'a':
		case 'A':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b01100110,
						              0b01111110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case 'b':
		case 'B':
		{
			const uint8_t letter_[8]={0b01111100,
						              0b01100110,
						              0b01100110,
						              0b01111100,
						              0b01100110,
						              0b01100110,
						              0b01111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'c':
		case 'C':
		{
			const uint8_t letter_[8]={0b00011110,
						              0b00110000,
						              0b01100000,
						              0b01100000,
						              0b01100000,
						              0b00110000,
						              0b00011110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'd':
		case 'D':
		{
			const uint8_t letter_[8]={0b01111000,
						              0b01101100,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01101100,
						              0b01111000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'e':
		case 'E':
		{
			const uint8_t letter_[8]={0b01111110,
						              0b01100000,
						              0b01100000,
						              0b01111000,
						              0b01100000,
						              0b01100000,
						              0b01111110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'f':
		case 'F':
		{
			const uint8_t letter_[8]={0b01111110,
						              0b01100000,
						              0b01100000,
						              0b01111000,
						              0b01100000,
						              0b01100000,
						              0b01100000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'g':
		case 'G':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b01100000,
						              0b01101110,
						              0b01100110,
						              0b01100110,
						              0b00111110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'h':
		case 'H':
		{
			const uint8_t letter_[8]={0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01111110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'i':
		case 'I':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'j':
		case 'J':
		{
			const uint8_t letter_[8]={0b00000110,
						              0b00000110,
						              0b00000110,
						              0b00000110,
						              0b00000110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'k':
		case 'K':
		{
			const uint8_t letter_[8]={0b11000110,
						              0b11001100,
						              0b11011000,
						              0b11110000,
						              0b11011000,
						              0b11001100,
						              0b11000110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'l':
		case 'L':
		{
			const uint8_t letter_[8]={0b01100000,
						              0b01100000,
						              0b01100000,
						              0b01100000,
						              0b01100000,
						              0b01100000,
						              0b01111110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'm':
		case 'M':
		{
			const uint8_t letter_[8]={0b11000110,
						              0b11101110,
						              0b11111110,
						              0b11010110,
						              0b11000110,
						              0b11000110,
						              0b11000110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'n':
		case 'N':
		{
			const uint8_t letter_[8]={0b11000110,
						              0b11100110,
						              0b11110110,
						              0b11011110,
						              0b11001110,
						              0b11000110,
						              0b11000110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'o':
		case 'O':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'p':
		case 'P':
		{
			const uint8_t letter_[8]={0b01111100,
						              0b01100110,
						              0b01100110,
						              0b01111100,
						              0b01100000,
						              0b01100000,
						              0b01100000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'q':
		case 'Q':
		{
			const uint8_t letter_[8]={0b01111000,
						              0b11001100,
						              0b11001100,
						              0b11001100,
						              0b11001100,
						              0b11011100,
						              0b01111110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'r':
		case 'R':
		{
			const uint8_t letter_[8]={0b01111100,
						              0b01100110,
						              0b01100110,
						              0b01111100,
						              0b01101100,
						              0b01100110,
						              0b01100110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 's':
		case 'S':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b01110000,
						              0b00111100,
						              0b00001110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 't':
		case 'T':
		{
			const uint8_t letter_[8]={0b01111110,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'u':
		case 'U':
		{
			const uint8_t letter_[8]={0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'v':
		case 'V':
		{
			const uint8_t letter_[8]={0b01100110,
						              0b01100110,
						              0b01100110,
						              0b01100110,
						              0b00111100,
						              0b00111100,
						              0b00011000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'w':
		case 'W':
		{
			const uint8_t letter_[8]={0b11000110,
						              0b11000110,
						              0b11000110,
						              0b11010110,
						              0b11111110,
						              0b11101110,
						              0b11000110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'x':
		case 'X':
		{
			const uint8_t letter_[8]={0b11000011,
						              0b01100110,
						              0b00111100,
						              0b00011000,
						              0b00111100,
						              0b01100110,
						              0b11000011,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'y':
		case 'Y':
		{
			const uint8_t letter_[8]={0b11000011,
						              0b01100110,
						              0b00111100,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case 'z':
		case 'Z':
		{
			const uint8_t letter_[8]={0b11111110,
						              0b00001100,
						              0b00011000,
						              0b00110000,
						              0b01100000,
						              0b11000000,
						              0b11111110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case '-':
		{
			const uint8_t letter_[8]={0b00000000,
						              0b00000000,
						              0b00000000,
									  0b01111110,
						              0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case '(':
		case '[':
		case '{':
		{
			const uint8_t letter_[8]={0b00001100,
						              0b00011000,
						              0b00110000,
									  0b00110000,
						              0b00110000,
						              0b00011000,
						              0b00001100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

        case ')':
		case ']':
		case '}':
		{
			const uint8_t letter_[8]={0b00110000,
						              0b00011000,
						              0b00001100,
									  0b00001100,
						              0b00001100,
						              0b00011000,
						              0b00110000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case ',':
		{
			const uint8_t letter_[8]={0b00000000,
						              0b00000000,
						              0b00000000,
									  0b00000000,
						              0b00000000,
						              0b00011000,
						              0b00011000,
						              0b00110000};
			memcpy(letter,letter_,8);
			break;
		}

		case '.':
		{
			const uint8_t letter_[8]={0b00000000,
						              0b00000000,
						              0b00000000,
									  0b00000000,
						              0b00000000,
						              0b00011000,
						              0b00011000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '!':
		{
			const uint8_t letter_[8]={0b00011000,
						              0b00011000,
						              0b00011000,
									  0b00011000,
						              0b00011000,
						              0b00000000,
						              0b00011000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '&':
		{
			const uint8_t letter_[8]={0b00111000,
						              0b01101100,
						              0b01101000,
									  0b01110110,
						              0b11011100,
						              0b11001110,
						              0b01111011,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '\'':
		{
			const uint8_t letter_[8]={0b00011000,
						              0b00011000,
						              0b00110000,
									  0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '0':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b01101110,
									  0b01111110,
						              0b01110110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '1':
		{
			const uint8_t letter_[8]={0b00011000,
						              0b00111000,
						              0b01111000,
									  0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '2':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b00000110,
									  0b00001100,
						              0b00011000,
						              0b00110000,
						              0b01111110,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '3':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b00000110,
									  0b00011100,
						              0b00000110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '4':
		{
			const uint8_t letter_[8]={0b00011100,
						              0b00111100,
						              0b01101100,
									  0b11001100,
						              0b11111110,
						              0b00001100,
						              0b00001100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '5':
		{
			const uint8_t letter_[8]={0b01111110,
						              0b01100000,
						              0b01111100,
									  0b00000110,
						              0b00000110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '6':
		{
			const uint8_t letter_[8]={0b00011100,
						              0b00110000,
						              0b01100000,
									  0b01111100,
						              0b01100110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '7':
		{
			const uint8_t letter_[8]={0b01111110,
						              0b00000110,
						              0b00000110,
									  0b00001100,
						              0b00011000,
						              0b00011000,
						              0b00011000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '8':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b01100110,
									  0b00111100,
						              0b01100110,
						              0b01100110,
						              0b00111100,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		case '9':
		{
			const uint8_t letter_[8]={0b00111100,
						              0b01100110,
						              0b01100110,
									  0b00111110,
						              0b00000110,
						              0b00001100,
						              0b00111000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}

		default:
		{
			const uint8_t letter_[8]={0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000,
						              0b00000000};
			memcpy(letter,letter_,8);
			break;
		}
	}
}

void mk_ili9225_get_letter(uint16_t *fbuf, char l, uint16_t color, uint16_t bgcolor) {
	uint8_t letter[8];
	uint8_t row;
	mk_ili9225_get_raw_glyph(letter, l);
	for(uint8_t y = 0; y < 8; y++) {
		row = letter[y];
		for(uint8_t x = 0; x < 8; x++) {
			if(row & 128) {
				fbuf[y * 8 + x] = color;
			} else {
				fbuf[y * 8 + x] = bgcolor;
			}
			row = row << 1;
		}
	}
}

/* ── Scale mode API ──────────────────────────────────────────────────────── */

void mk_ili9225_set_scale_mode(ili9341_scale_mode_t mode)
{
    st7789_wait_for_idle();

    static int s_last_gb_mode = -1;
    int current_gb_mode = dmg_is_gb_mode() ? 1 : 0;

    if (mode == st7789_scale_mode && scale_x_map_inited && s_last_gb_mode == current_gb_mode) {
        return; /* No change – avoid redundant recomputation. */
    }

    s_last_gb_mode = current_gb_mode;
    st7789_scale_mode = mode;

    if (dmg_is_gb_mode()) {
        switch (mode) {
        case SCALE_INTEGER:
            /* Integer (1x nativo 128x128) centralizado no visor da carcaça DMG (x=45..310, largura 266) */
            cur_render_width  = 128u;
            cur_render_height = 128u;
            cur_render_x      = (uint16_t)(DMG_GB_WINDOW_X + (DMG_GB_WINDOW_W - 128u) / 2u); /* 45 + 69 = 114 */
            cur_render_y      = (uint16_t)((240u - 128u) / 2u);                              /* 56 */
            break;
        case SCALE_STRETCH:
            /* Stretch (tela cheia na janela DMG de 266x240) */
            cur_render_width  = DMG_GB_WINDOW_W;                                            /* 266 */
            cur_render_height = 240u;
            cur_render_x      = DMG_GB_WINDOW_X;                                            /* 45 */
            cur_render_y      = 0u;
            break;
        case SCALE_FIT:
        default:
            /* Fit (resolução nativa escalada 1:1, 240x240, centralizada na janela DMG) */
            cur_render_width  = 240u;
            cur_render_height = 240u;
            cur_render_x      = (uint16_t)(DMG_GB_WINDOW_X + (DMG_GB_WINDOW_W - 240u) / 2u); /* 45 + 13 = 58 */
            cur_render_y      = 0u;
            break;
        }
    } else {
        switch (mode) {
        case SCALE_INTEGER:
            /* Integer (1x nativo 128x128) centralizado no display total 320x240 */
            cur_render_width  = 128u;
            cur_render_height = 128u;
            cur_render_x      = (uint16_t)((ST7789_WIDTH  - 128u) / 2u);                     /* 96 */
            cur_render_y      = (uint16_t)((ST7789_HEIGHT - 128u) / 2u);                     /* 56 */
            break;
        case SCALE_STRETCH:
            /* Stretch (tela cheia total 320x240) */
            cur_render_width  = ST7789_WIDTH;                                               /* 320 */
            cur_render_height = ST7789_HEIGHT;                                              /* 240 */
            cur_render_x      = 0u;
            cur_render_y      = 0u;
            break;
        case SCALE_FIT:
        default:
            /* Fit (resolução nativa escalada 1:1, 240x240, centralizada no display) */
            cur_render_width  = 240u;
            cur_render_height = 240u;
            cur_render_x      = (uint16_t)((ST7789_WIDTH  - 240u) / 2u);                     /* 40 */
            cur_render_y      = 0u;
            break;
        }
    }

    /* Recompute the lookup tables for the new render width/height. */
    init_scale_x_map();
    init_scale_y_map();
    mk_ili9225_fill(0x0000);
}

ili9341_scale_mode_t mk_ili9225_get_scale_mode(void)
{
    return st7789_scale_mode;
}

/* ── draw_dimmed_frame ───────────────────────────────────────────────────── */

/* Helper: map a screen pixel (sx, sy) back to the source 160x144 frame and
 * return a 50%-darkened version of that colour. */
static uint16_t st7789_get_dimmed_pixel(uint16_t sx, uint16_t sy,
                                        const uint16_t *src_160x144)
{
    uint16_t gb_x, gb_y;
    uint16_t c;

    /* Inverse-map screen → GB frame depending on the current scale mode. */
    if (sx < cur_render_x || sx >= (uint16_t)(cur_render_x + cur_render_width) ||
        sy < cur_render_y || sy >= (uint16_t)(cur_render_y + cur_render_height)) {
        return 0x0000u; /* Outside the game viewport — black. */
    }

    switch (st7789_scale_mode) {
    case SCALE_INTEGER:
        gb_x = (uint16_t)(sx - cur_render_x);
        gb_y = (uint16_t)(sy - cur_render_y);
        break;
    case SCALE_STRETCH:
    case SCALE_FIT:
    default:
        gb_x = (uint16_t)(((uint32_t)(sx - cur_render_x) * GB_LCD_WIDTH)  / cur_render_width);
        gb_y = (uint16_t)(((uint32_t)(sy - cur_render_y) * GB_LCD_HEIGHT) / cur_render_height);
        break;
    }

    if (gb_x >= GB_LCD_WIDTH || gb_y >= GB_LCD_HEIGHT) {
        return 0x0000u;
    }

    c = src_160x144[(uint16_t)(gb_y * GB_LCD_WIDTH) + gb_x];

    /* 50% dimming: shift each channel right by 1. */
    return (uint16_t)(((c & 0xF800u) >> 1u) |
                      ((c & 0x07E0u) >> 1u) |
                      ((c & 0x001Fu) >> 1u));
}

void mk_ili9225_draw_dimmed_frame(const uint16_t *src_160x144)
{
    static uint8_t row_buf[ST7789_WIDTH * 2u];
    uint16_t sy;

    st7789_wait_for_idle();
    st7789_set_addr_window_raw(0u, 0u, ST7789_WIDTH - 1u, ST7789_HEIGHT - 1u);
    st7789_select(true);
    st7789_dc(true);

    for (sy = 0u; sy < ST7789_HEIGHT; ++sy) {
        uint16_t sx;
        for (sx = 0u; sx < ST7789_WIDTH; ++sx) {
            uint16_t c = st7789_get_dimmed_pixel(sx, sy, src_160x144);
            st7789_pack_rgb565(c, &row_buf[sx * 2u]);
        }
        spi_write_blocking(spi0, row_buf, ST7789_WIDTH * 2u);
    }

    st7789_select(false);
}

/* ── text_transparent ────────────────────────────────────────────────────── */

void mk_ili9225_text_transparent(const char *s, uint16_t x, uint16_t y,
                                  uint16_t color,
                                  const uint16_t *src_160x144)
{
    static uint8_t row_buf[8u * 2u];
    size_t len;
    size_t ci;

    if (s == NULL) {
        return;
    }

    len = strlen(s);
    for (ci = 0u; ci < len; ++ci) {
        uint8_t glyph[8];
        uint16_t gx = (uint16_t)(x + (uint16_t)(ci * 8u));
        uint8_t gy;

        if (gx + 8u > ST7789_WIDTH) {
            break;
        }

        mk_ili9225_get_raw_glyph(glyph, s[ci]);

        st7789_wait_for_idle();
        st7789_set_addr_window_raw(gx, y, (uint16_t)(gx + 7u), (uint16_t)(y + 7u));
        st7789_select(true);
        st7789_dc(true);

        for (gy = 0u; gy < 8u; ++gy) {
            uint8_t row = glyph[gy];
            uint8_t bit;
            for (bit = 0u; bit < 8u; ++bit) {
                uint16_t c;
                if (row & 0x80u) {
                    c = color;
                } else {
                    /* Background: sample the dimmed game frame. */
                    c = st7789_get_dimmed_pixel((uint16_t)(gx + bit),
                                               (uint16_t)(y + gy),
                                               src_160x144);
                }
                st7789_pack_rgb565(c, &row_buf[bit * 2u]);
                row <<= 1u;
            }
            spi_write_blocking(spi0, row_buf, 8u * 2u);
        }

        st7789_select(false);
    }
}
void mk_ili9225_text(char *s, uint16_t x, uint16_t y, uint16_t color, uint16_t bgcolor) {
	uint16_t fbuf[8*8];
	for(uint8_t i=0;i<strlen(s);i++) {
		mk_ili9225_get_letter(fbuf,s[i],color,bgcolor);
		mk_ili9225_blit(fbuf,x,y,8,8);
		x+=8;
		if(x>(ST7789_WIDTH-8)) {
			break;
		}
	}
}

void mk_ili9225_render_volume_hud(void)
{
    /* No-op: HUD is now stamped directly into band scanlines in mk_ili9225_write_pixels */
}

