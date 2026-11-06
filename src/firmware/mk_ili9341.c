/**
 * ILI9341 compatibility layer that exposes the mk_ili9225_* API used by
 * main.c. This allows the emulator core to run on ILI9341 320x240 SPI panels.
 *
 * Sequence and timing match the verified driver in pico-bootLoader.
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

/* GPIO mapping for LCD pins. */
#define ILI9341_GPIO_CS   17u
#define ILI9341_GPIO_DC   20u
#define ILI9341_GPIO_RST  21u
#define ILI9341_GPIO_LED  22u

/* Game Boy source frame dimensions. */
#define GB_LCD_WIDTH      160u
#define GB_LCD_HEIGHT     144u

/* ILI9341 320x240 landscape. */
#define ILI9341_WIDTH     320u
#define ILI9341_HEIGHT    240u

#define GB_MAX_RENDER_WIDTH   320u
#define GB_MAX_RENDER_HEIGHT  240u

static ili9341_scale_mode_t ili9341_scale_mode = SCALE_FIT;

static inline uint16_t get_render_width(void) {
    if (dmg_is_gb_mode()) {
        switch (ili9341_scale_mode) {
            case SCALE_INTEGER: return 160u;
            case SCALE_STRETCH:
            case SCALE_FIT:
            default:            return DMG_GB_WINDOW_W;
        }
    } else {
        switch (ili9341_scale_mode) {
            case SCALE_STRETCH: return 320u;
            case SCALE_INTEGER: return 160u;
            case SCALE_FIT:
            default:            return 266u;
        }
    }
}

static inline uint16_t get_render_height(void) {
    switch (ili9341_scale_mode) {
        case SCALE_INTEGER: return 144u;
        case SCALE_STRETCH:
        case SCALE_FIT:
        default:            return 240u;
    }
}

static inline uint16_t get_render_x(void) {
    if (dmg_is_gb_mode()) {
        switch (ili9341_scale_mode) {
            case SCALE_INTEGER: return (uint16_t)(DMG_GB_WINDOW_X + (DMG_GB_WINDOW_W - 160u) / 2u); /* 98u */
            case SCALE_STRETCH:
            case SCALE_FIT:
            default:            return DMG_GB_WINDOW_X; /* 45u */
        }
    } else {
        switch (ili9341_scale_mode) {
            case SCALE_STRETCH: return 0u;
            case SCALE_INTEGER: return 80u;
            case SCALE_FIT:
            default:            return 27u;
        }
    }
}

static inline uint16_t get_render_y(void) {
    switch (ili9341_scale_mode) {
        case SCALE_INTEGER: return 48u;
        case SCALE_STRETCH:
        case SCALE_FIT:
        default:            return 0u;
    }
}

/* ILI9341 commands. */
#define CMD_NOP           0x00
#define CMD_SWRESET       0x01
#define CMD_SLPIN         0x10
#define CMD_SLPOUT        0x11
#define CMD_NORON         0x13
#define CMD_INVOFF        0x20
#define CMD_INVON         0x21
#define CMD_GAMSET        0x26
#define CMD_DISPOFF       0x28
#define CMD_DISPON        0x29
#define CMD_CASET         0x2A
#define CMD_PASET         0x2B
#define CMD_RAMWR         0x2C
#define CMD_MADCTL        0x36
#define CMD_COLMOD        0x3A
#define CMD_FRMCTR1       0xB1
#define CMD_INVCTR        0xB4
#define CMD_DFUNCTR       0xB6
#define CMD_PWCTR1        0xC0
#define CMD_PWCTR2        0xC1
#define CMD_PWCTR3        0xC2
#define CMD_PWCTR4        0xC3
#define CMD_PWCTR5        0xC4
#define CMD_VMCTR1        0xC5
#define CMD_VMCTR2        0xC7
#define CMD_GMCTRP1       0xE0
#define CMD_GMCTRN1       0xE1

#ifndef ILI9341_MADCTL_VALUE
#define ILI9341_MADCTL_VALUE 0x28  /* Landscape 320x240, RGB order, row/col exchange */
#endif

#define ILI9341_DMA_BAND_LINES 8u
#define ILI9341_DMA_QUEUE_DEPTH 4u

typedef enum {
    ILI9341_SLOT_FREE = 0,
    ILI9341_SLOT_QUEUED,
    ILI9341_SLOT_SENDING
} ili9341_slot_state_e;

typedef struct {
    uint8_t buffer[ILI9341_DMA_BAND_LINES * GB_MAX_RENDER_WIDTH * 2u];
    size_t len;
    uint16_t x0;
    uint16_t y0;
    uint16_t x1;
    uint16_t y1;
    volatile ili9341_slot_state_e state;
} ili9341_dma_slot_t;

static uint8_t ili9341_line_scaled[GB_MAX_RENDER_WIDTH * 2u];
static uint8_t ili9341_line_scaled_grid[GB_MAX_RENDER_WIDTH * 2u];
static bool ili9341_grid_enabled = false;
static ili9341_dma_slot_t ili9341_dma_slots[ILI9341_DMA_QUEUE_DEPTH];
static uint_fast8_t ili9341_dma_write_idx = 0u;
static uint_fast8_t ili9341_dma_read_idx = 0u;
static int ili9341_dma_chan = -1;
static volatile bool ili9341_dma_active = false;
static bool ili9341_dma_initialized = false;

static uint8_t ili9341_band_buffer[ILI9341_DMA_BAND_LINES * GB_MAX_RENDER_WIDTH * 2u];
static size_t ili9341_band_len = 0u;
static uint16_t ili9341_band_y0 = 0u;
static uint16_t ili9341_band_y1 = 0u;
static uint16_t ili9341_band_rows = 0u;

static int ili9341_current_y = -1;

static void ili9341_dma_start_next_locked(void);
static void ili9341_wait_for_idle(void);

static inline void ili9341_select(bool selected)
{
    gpio_put(ILI9341_GPIO_CS, !selected);
}

static inline void ili9341_dc(bool data_mode)
{
    gpio_put(ILI9341_GPIO_DC, data_mode);
}

static void ili9341_write_command_raw(uint8_t cmd)
{
    while (spi_is_busy(spi0)) tight_loop_contents();
    ili9341_select(true);
    ili9341_dc(false);
    spi_write_blocking(spi0, &cmd, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();
    ili9341_select(false);
}

static void ili9341_write_data_raw(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    while (spi_is_busy(spi0)) tight_loop_contents();
    ili9341_select(true);
    ili9341_dc(true);
    spi_write_blocking(spi0, data, len);
    while (spi_is_busy(spi0)) tight_loop_contents();
    ili9341_select(false);
}

static void ili9341_write_command_data_raw(uint8_t cmd, const uint8_t *data, size_t len)
{
    ili9341_write_command_raw(cmd);
    ili9341_write_data_raw(data, len);
}

static void ili9341_set_addr_window_raw(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t caset[4];
    uint8_t paset[4];

    caset[0] = (uint8_t)(x0 >> 8);
    caset[1] = (uint8_t)(x0 & 0xFF);
    caset[2] = (uint8_t)(x1 >> 8);
    caset[3] = (uint8_t)(x1 & 0xFF);

    paset[0] = (uint8_t)(y0 >> 8);
    paset[1] = (uint8_t)(y0 & 0xFF);
    paset[2] = (uint8_t)(y1 >> 8);
    paset[3] = (uint8_t)(y1 & 0xFF);

    while (spi_is_busy(spi0)) tight_loop_contents();

    ili9341_select(true);

    ili9341_dc(false);
    spi_write_blocking(spi0, &(uint8_t){CMD_CASET}, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();

    ili9341_dc(true);
    spi_write_blocking(spi0, caset, sizeof(caset));
    while (spi_is_busy(spi0)) tight_loop_contents();

    ili9341_dc(false);
    spi_write_blocking(spi0, &(uint8_t){CMD_PASET}, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();

    ili9341_dc(true);
    spi_write_blocking(spi0, paset, sizeof(paset));
    while (spi_is_busy(spi0)) tight_loop_contents();

    ili9341_dc(false);
    spi_write_blocking(spi0, &(uint8_t){CMD_RAMWR}, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();

    ili9341_dc(true);
}

static inline void ili9341_pack_rgb565(uint16_t c, uint8_t *dst)
{
    dst[0] = (uint8_t)(c >> 8);
    dst[1] = (uint8_t)(c & 0xFFu);
}

static uint8_t scale_x_map[GB_MAX_RENDER_WIDTH];
static bool scale_x_map_inited = false;

static void init_scale_x_map(void)
{
    uint16_t w = get_render_width();
    for (uint16_t x_out = 0; x_out < w; ++x_out) {
        scale_x_map[x_out] = (uint8_t)(((uint32_t)x_out * GB_LCD_WIDTH) / w);
    }
    scale_x_map_inited = true;
}

static inline uint16_t ili9341_grid_darken(uint16_t c)
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

static void ili9341_scale_line(const uint16_t *src)
{
    if (!scale_x_map_inited) {
        init_scale_x_map();
    }
    uint16_t w = get_render_width();
    for (uint16_t x_out = 0; x_out < w; ++x_out) {
        const uint16_t c = src[scale_x_map[x_out]];
        const bool x_line = (ili9341_grid_enabled && ((x_out & 1u) == 1u));
        ili9341_pack_rgb565(x_line ? ili9341_grid_darken(c) : c,
                           &ili9341_line_scaled[(size_t)x_out * 2u]);
        ili9341_pack_rgb565(ili9341_grid_enabled ? ili9341_grid_darken(c) : c,
                           &ili9341_line_scaled_grid[(size_t)x_out * 2u]);
    }
}

static bool ili9341_dma_queue_busy(void)
{
    if (ili9341_dma_active) return true;
    for (uint_fast8_t i = 0; i < ILI9341_DMA_QUEUE_DEPTH; ++i) {
        if (ili9341_dma_slots[i].state != ILI9341_SLOT_FREE) return true;
    }
    return false;
}

static void ili9341_flush_pending_band(void)
{
    if (ili9341_band_rows == 0u || ili9341_band_len == 0u) return;

    const uint16_t x0 = get_render_x();
    const uint16_t x1 = (uint16_t)(x0 + get_render_width() - 1u);
    const uint16_t y0 = (uint16_t)(get_render_y() + ili9341_band_y0);
    const uint16_t y1 = (uint16_t)(get_render_y() + ili9341_band_y1 - 1u);

    if (!ili9341_dma_initialized) {
        ili9341_set_addr_window_raw(x0, y0, x1, y1);
        ili9341_select(true);
        ili9341_dc(true);
        spi_write_blocking(spi0, ili9341_band_buffer, ili9341_band_len);
        ili9341_select(false);
    } else {
        ili9341_dma_slot_t *slot = &ili9341_dma_slots[ili9341_dma_write_idx];
        while (slot->state != ILI9341_SLOT_FREE) {
            tight_loop_contents();
        }

        memcpy(slot->buffer, ili9341_band_buffer, ili9341_band_len);
        slot->len = ili9341_band_len;
        slot->x0 = x0;
        slot->y0 = y0;
        slot->x1 = x1;
        slot->y1 = y1;

        uint32_t status = save_and_disable_interrupts();
        slot->state = ILI9341_SLOT_QUEUED;
        if (!ili9341_dma_active) {
            ili9341_dma_start_next_locked();
        }
        restore_interrupts(status);

        ili9341_dma_write_idx = (uint_fast8_t)((ili9341_dma_write_idx + 1u) % ILI9341_DMA_QUEUE_DEPTH);
    }

    ili9341_band_rows = 0u;
    ili9341_band_len = 0u;
    ili9341_band_y0 = 0u;
    ili9341_band_y1 = 0u;
}

static void ili9341_wait_for_idle(void)
{
    if (!ili9341_dma_initialized) return;
    ili9341_flush_pending_band();
    while (ili9341_dma_queue_busy()) tight_loop_contents();
    while (spi_is_busy(spi0)) tight_loop_contents();
}

static void ili9341_write_command(uint8_t cmd)
{
    ili9341_wait_for_idle();
    ili9341_write_command_raw(cmd);
}

static void ili9341_write_data(const uint8_t *data, size_t len)
{
    ili9341_wait_for_idle();
    ili9341_write_data_raw(data, len);
}

static void ili9341_write_command_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    ili9341_wait_for_idle();
    ili9341_write_command_data_raw(cmd, data, len);
}

static void ili9341_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    ili9341_wait_for_idle();
    ili9341_set_addr_window_raw(x0, y0, x1, y1);
}

static void ili9341_dma_start_next_locked(void)
{
    ili9341_dma_slot_t *slot = &ili9341_dma_slots[ili9341_dma_read_idx];
    if (slot->state != ILI9341_SLOT_QUEUED) {
        ili9341_dma_active = false;
        return;
    }

    ili9341_dma_active = true;
    slot->state = ILI9341_SLOT_SENDING;

    while (spi_is_busy(spi0)) tight_loop_contents();

    ili9341_set_addr_window_raw(slot->x0, slot->y0, slot->x1, slot->y1);
    ili9341_select(true);
    ili9341_dc(true);

    dma_channel_set_read_addr(ili9341_dma_chan, slot->buffer, false);
    dma_channel_set_trans_count(ili9341_dma_chan, (uint32_t)slot->len, true);
}

static void ili9341_dma_irq_handler(void)
{
    if (ili9341_dma_chan < 0) return;
    if (!dma_channel_get_irq0_status(ili9341_dma_chan)) return;

    dma_channel_acknowledge_irq0(ili9341_dma_chan);

    ili9341_dma_slot_t *slot = &ili9341_dma_slots[ili9341_dma_read_idx];
    slot->state = ILI9341_SLOT_FREE;
    ili9341_dma_read_idx = (uint_fast8_t)((ili9341_dma_read_idx + 1u) % ILI9341_DMA_QUEUE_DEPTH);

    while (spi_is_busy(spi0)) tight_loop_contents();
    ili9341_select(false);

    ili9341_dma_start_next_locked();
}

static void ili9341_dma_init(void)
{
    if (ili9341_dma_initialized) return;

    ili9341_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ili9341_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(ili9341_dma_chan, &c, &spi_get_hw(spi0)->dr, NULL, 0, false);

    dma_channel_set_irq0_enabled(ili9341_dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, ili9341_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    ili9341_dma_initialized = true;
}

static void ili9341_hw_reset(void)
{
    gpio_put(ILI9341_GPIO_RST, true);
    sleep_ms(10);
    gpio_put(ILI9341_GPIO_RST, false);
    sleep_ms(20);
    gpio_put(ILI9341_GPIO_RST, true);
    sleep_ms(150);
}

static void ili9341_init_sequence(void)
{
    ili9341_write_command(CMD_SWRESET);
    sleep_ms(150);

    const uint8_t pwctr1[] = {0x23, 0x00};
    ili9341_write_command_data(CMD_PWCTR1, pwctr1, sizeof(pwctr1));

    const uint8_t pwctr2[] = {0x10};
    ili9341_write_command_data(CMD_PWCTR2, pwctr2, sizeof(pwctr2));

    const uint8_t vmctr1[] = {0x3E, 0x28};
    ili9341_write_command_data(CMD_VMCTR1, vmctr1, sizeof(vmctr1));

    const uint8_t vmctr2[] = {0x86};
    ili9341_write_command_data(CMD_VMCTR2, vmctr2, sizeof(vmctr2));

    const uint8_t madctl[] = {(uint8_t)ILI9341_MADCTL_VALUE};
    ili9341_write_command_data(CMD_MADCTL, madctl, sizeof(madctl));

    const uint8_t colmod[] = {0x55}; /* 16-bit RGB565 */
    ili9341_write_command_data(CMD_COLMOD, colmod, sizeof(colmod));

    const uint8_t frmctr1[] = {0x00, 0x18};
    ili9341_write_command_data(CMD_FRMCTR1, frmctr1, sizeof(frmctr1));

    const uint8_t dfunctr[] = {0x08, 0x82, 0x27};
    ili9341_write_command_data(CMD_DFUNCTR, dfunctr, sizeof(dfunctr));

    const uint8_t gamset[] = {0x01};
    ili9341_write_command_data(CMD_GAMSET, gamset, sizeof(gamset));

    const uint8_t invctr[] = {0x00};
    ili9341_write_command_data(CMD_INVCTR, invctr, sizeof(invctr));

    const uint8_t pwctr3[] = {0x0A, 0x00};
    ili9341_write_command_data(CMD_PWCTR3, pwctr3, sizeof(pwctr3));

    const uint8_t pwctr4[] = {0x8A, 0x2A};
    ili9341_write_command_data(CMD_PWCTR4, pwctr4, sizeof(pwctr4));

    const uint8_t pwctr5[] = {0x8A, 0xEE};
    ili9341_write_command_data(CMD_PWCTR5, pwctr5, sizeof(pwctr5));

    const uint8_t vmctr1_2[] = {0x0E};
    ili9341_write_command_data(CMD_VMCTR1, vmctr1_2, sizeof(vmctr1_2));

    static const uint8_t gamma_p[] = {
        0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
        0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00
    };
    ili9341_write_command_data(CMD_GMCTRP1, gamma_p, sizeof(gamma_p));

    static const uint8_t gamma_n[] = {
        0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
        0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F
    };
    ili9341_write_command_data(CMD_GMCTRN1, gamma_n, sizeof(gamma_n));

    ili9341_write_command(CMD_SLPOUT);
    sleep_ms(120);

    ili9341_set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    ili9341_write_command(CMD_DISPON);
    sleep_ms(120);
}

/* =========================================================================
 * Implementation of mk_ili9225_* interface
 * ========================================================================= */

unsigned mk_ili9225_init(void)
{
    printf("LCD: ILI9341 init (320x240, MADCTL=0x%02X)\n",
           (unsigned)((uint8_t)ILI9341_MADCTL_VALUE));

    gpio_put(ILI9341_GPIO_LED, false);
    ili9341_select(false);
    ili9341_dc(true);
    ili9341_dma_init();

    ili9341_hw_reset();
    ili9341_init_sequence();

    mk_ili9225_fill(0x0000);
    gpio_put(ILI9341_GPIO_LED, true);
    return 0;
}

void mk_ili9225_display_control(bool invert, ili9225_color_mode_e colour_mode)
{
    (void)colour_mode;
    ili9341_write_command(invert ? CMD_INVON : CMD_INVOFF);
}

void mk_ili9225_invert_display(bool invert)
{
    mk_ili9225_display_control(invert, ILI9225_COLOR_MODE_FULL);
}

void mk_ili9225_set_window(uint16_t hor_start, uint16_t hor_end,
                           uint16_t vert_start, uint16_t vert_end)
{
    if (hor_start > hor_end || vert_start > vert_end) return;
    if (hor_start >= ILI9341_WIDTH || vert_start >= ILI9341_HEIGHT) return;
    if (hor_end >= ILI9341_WIDTH) hor_end = ILI9341_WIDTH - 1;
    if (vert_end >= ILI9341_HEIGHT) vert_end = ILI9341_HEIGHT - 1;

    ili9341_set_addr_window(hor_start, vert_start, hor_end, vert_end);
}

void mk_ili9225_set_address(uint8_t x, uint8_t y)
{
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;
    ili9341_set_addr_window(x, y, x, y);
}

void mk_ili9225_set_x(uint8_t x)
{
    int gb_line = (int)x - 16;
    if (gb_line < 0 || gb_line >= (int)GB_LCD_HEIGHT) {
        ili9341_current_y = -1;
        return;
    }
    ili9341_current_y = gb_line;
}

void mk_ili9225_write_pixels(const uint16_t *pixels, uint_fast16_t nmemb)
{
    if (pixels == NULL || nmemb == 0 || ili9341_current_y < 0) return;
    if (nmemb < GB_LCD_WIDTH) return;

    ili9341_scale_line(pixels);

    uint16_t w = get_render_width();
    uint16_t h = get_render_height();
    uint16_t y0, y1;
    if (ili9341_scale_mode == SCALE_INTEGER) {
        y0 = (uint16_t)ili9341_current_y;
        y1 = y0 + 1u;
    } else {
        y0 = (uint16_t)(((uint32_t)ili9341_current_y * h) / GB_LCD_HEIGHT);
        y1 = (uint16_t)(((uint32_t)(ili9341_current_y + 1) * h) / GB_LCD_HEIGHT);
        if (y1 <= y0) y1 = y0 + 1u;
        if (y1 > h) y1 = h;
    }

    uint16_t rows = (uint16_t)(y1 - y0);
    if (rows == 0u) return;

    if (ili9341_band_rows != 0u &&
        (y0 != ili9341_band_y1 ||
         (uint16_t)(ili9341_band_rows + rows) > ILI9341_DMA_BAND_LINES)) {
        ili9341_flush_pending_band();
    }

    if (ili9341_band_rows == 0u) {
        ili9341_band_y0 = y0;
        ili9341_band_y1 = y0;
        ili9341_band_len = 0u;
    }

    size_t line_bytes = (size_t)w * 2u;
    for (uint16_t row = 0; row < rows; ++row) {
        uint16_t screen_y = (uint16_t)(get_render_y() + y0 + row);
        const bool y_line = (ili9341_grid_enabled && (((uint16_t)(y0 + row) & 1u) == 1u));
        const uint8_t *line = y_line ? ili9341_line_scaled_grid : ili9341_line_scaled;
        uint8_t *dst = &ili9341_band_buffer[ili9341_band_len];
        memcpy(dst, line, line_bytes);

        /* Volume OSD HUD scanline stamp */
        int hud_x, hud_w;
        const uint16_t *hud = dmg_volume_hud_get_row(screen_y, &hud_x, &hud_w);
        if (hud) {
            int render_x0 = (int)get_render_x();
            int render_w = (int)w;
            for (int i = 0; i < hud_w; i++) {
                int local_x = (hud_x + i) - render_x0;
                if (local_x >= 0 && local_x < render_w) {
                    uint16_t c = hud[i];
                    if (c != 0x0001) {
                        dst[local_x * 2 + 0] = (uint8_t)(c >> 8);
                        dst[local_x * 2 + 1] = (uint8_t)(c & 0xFFu);
                    }
                }
            }
        }

        ili9341_band_len += line_bytes;
        ili9341_band_rows++;
        ili9341_band_y1++;
    }

    if (ili9341_band_rows >= ILI9341_DMA_BAND_LINES ||
        ili9341_current_y == (int)(GB_LCD_HEIGHT - 1u)) {
        ili9341_flush_pending_band();
        if (ili9341_current_y == (int)(GB_LCD_HEIGHT - 1u) && ili9341_scale_mode == SCALE_INTEGER) {
            if (dmg_volume_hud_is_active()) {
                int hx, hw;
                const uint16_t *row192 = dmg_volume_hud_get_row(192, &hx, &hw);
                if (row192 && hw > 0) {
                    static uint8_t s_hud_bot[8 * 160 * 2];
                    for (int r = 0; r < 8; r++) {
                        const uint16_t *hrow = dmg_volume_hud_get_row((uint16_t)(192 + r), NULL, NULL);
                        if (!hrow) break;
                        for (int c = 0; c < hw; c++) {
                            uint16_t col = hrow[c];
                            s_hud_bot[(r * hw + c) * 2 + 0] = (uint8_t)(col >> 8);
                            s_hud_bot[(r * hw + c) * 2 + 1] = (uint8_t)(col & 0xFF);
                        }
                    }
                    ili9341_wait_for_idle();
                    ili9341_set_addr_window_raw((uint16_t)hx, 192, (uint16_t)(hx + hw - 1), 199);
                    ili9341_select(true);
                    ili9341_dc(true);
                    spi_write_blocking(spi0, s_hud_bot, (size_t)hw * 8u * 2u);
                    while (spi_is_busy(spi0)) tight_loop_contents();
                    ili9341_select(false);
                }
            }
        }
    }
}

void mk_ili9225_write_pixels_start(void)
{
}

void mk_ili9225_write_pixels_end(void)
{
    ili9341_flush_pending_band();
}

void mk_ili9225_power_control(uint8_t drive_power, bool sleep)
{
    (void)drive_power;
    ili9341_write_command(sleep ? CMD_SLPIN : CMD_SLPOUT);
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

void mk_ili9225_set_grid_enabled(bool enabled)
{
    ili9341_grid_enabled = enabled;
}

bool mk_ili9225_get_grid_enabled(void)
{
    return ili9341_grid_enabled;
}

void mk_ili9225_set_scale_mode(ili9341_scale_mode_t mode)
{
    static int s_last_gb_mode = -1;
    int current_gb_mode = dmg_is_gb_mode() ? 1 : 0;
    if (mode > SCALE_INTEGER) mode = SCALE_FIT;
    if (ili9341_scale_mode != mode || s_last_gb_mode != current_gb_mode || !scale_x_map_inited) {
        ili9341_wait_for_idle();
        ili9341_scale_mode = mode;
        s_last_gb_mode = current_gb_mode;
        scale_x_map_inited = false;
        init_scale_x_map();
        mk_ili9225_fill(0x0000);
    }
}

ili9341_scale_mode_t mk_ili9225_get_scale_mode(void)
{
    return ili9341_scale_mode;
}

void mk_ili9225_exit(void)
{
    ili9341_write_command(CMD_DISPOFF);
}

void mk_ili9225_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    static uint8_t color_block[128];
    if (w == 0 || h == 0) return;
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;

    uint16_t x0 = x;
    uint16_t y0 = y;
    uint16_t x1 = (uint16_t)(x0 + w - 1);
    uint16_t y1 = (uint16_t)(y0 + h - 1);
    if (x1 >= ILI9341_WIDTH) x1 = ILI9341_WIDTH - 1;
    if (y1 >= ILI9341_HEIGHT) y1 = ILI9341_HEIGHT - 1;

    ili9341_pack_rgb565(color, &color_block[0]);
    for (size_t i = 2; i < sizeof(color_block); i += 2) {
        color_block[i] = color_block[0];
        color_block[i + 1] = color_block[1];
    }

    ili9341_set_addr_window(x0, y0, x1, y1);
    ili9341_select(true);
    ili9341_dc(true);

    uint32_t pixels = ((uint32_t)x1 - x0 + 1u) * ((uint32_t)y1 - y0 + 1u);
    while (pixels != 0u) {
        uint32_t chunk_pixels = (uint32_t)(sizeof(color_block) / 2u);
        if (chunk_pixels > pixels) chunk_pixels = pixels;
        spi_write_blocking(spi0, color_block, chunk_pixels * 2u);
        pixels -= chunk_pixels;
    }
    ili9341_select(false);
}

void mk_ili9225_fill(uint16_t color)
{
    static uint8_t color_block[128];
    ili9341_pack_rgb565(color, &color_block[0]);
    for (size_t i = 2; i < sizeof(color_block); i += 2) {
        color_block[i] = color_block[0];
        color_block[i + 1] = color_block[1];
    }

    ili9341_set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    ili9341_select(true);
    ili9341_dc(true);

    uint32_t pixels = (uint32_t)ILI9341_WIDTH * (uint32_t)ILI9341_HEIGHT;
    while (pixels != 0u) {
        uint32_t chunk_pixels = (uint32_t)(sizeof(color_block) / 2u);
        if (chunk_pixels > pixels) chunk_pixels = pixels;
        spi_write_blocking(spi0, color_block, chunk_pixels * 2u);
        pixels -= chunk_pixels;
    }
    ili9341_select(false);
}

void mk_ili9225_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    mk_ili9225_fill_rect(x, y, 1, 1, color);
}

void mk_ili9225_blit(uint16_t *fbuf, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    static uint8_t txbuf[8 * 8 * 2];
    if (fbuf == NULL || w == 0 || h == 0) return;
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;

    uint16_t x0 = x;
    uint16_t y0 = y;
    uint16_t x1 = (uint16_t)(x0 + w - 1);
    uint16_t y1 = (uint16_t)(y0 + h - 1);
    if (x1 >= ILI9341_WIDTH) x1 = ILI9341_WIDTH - 1;
    if (y1 >= ILI9341_HEIGHT) y1 = ILI9341_HEIGHT - 1;

    uint32_t pixels = ((uint32_t)x1 - x0 + 1u) * ((uint32_t)y1 - y0 + 1u);
    ili9341_set_addr_window(x0, y0, x1, y1);

    ili9341_select(true);
    ili9341_dc(true);

    uint32_t sent = 0;
    while (sent < pixels) {
        uint32_t chunk_pixels = (uint32_t)(sizeof(txbuf) / 2u);
        if (chunk_pixels > (pixels - sent)) chunk_pixels = pixels - sent;

        for (uint32_t i = 0; i < chunk_pixels; ++i) {
            uint16_t c = fbuf[sent + i];
            ili9341_pack_rgb565(c, &txbuf[i * 2u]);
        }

        spi_write_blocking(spi0, txbuf, chunk_pixels * 2u);
        sent += chunk_pixels;
    }
    ili9341_select(false);
}

/* Character font generator for ASCII characters. */
void mk_ili9225_get_raw_glyph(uint8_t d[8], char l)
{
    memset(d, 0, 8);
    switch (l) {
        case 'a': case 'A': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100110,0b01111110,0b01100110,0b01100110,0b01100110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'b': case 'B': {
            const uint8_t b[8] = {0b01111100,0b01100110,0b01100110,0b01111100,0b01100110,0b01100110,0b01111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'c': case 'C': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100000,0b01100000,0b01100000,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'd': case 'D': {
            const uint8_t b[8] = {0b01111000,0b01101100,0b01100110,0b01100110,0b01100110,0b01101100,0b01111000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'e': case 'E': {
            const uint8_t b[8] = {0b01111110,0b01100000,0b01100000,0b01111000,0b01100000,0b01100000,0b01111110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'f': case 'F': {
            const uint8_t b[8] = {0b01111110,0b01100000,0b01100000,0b01111000,0b01100000,0b01100000,0b01100000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'g': case 'G': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100000,0b01101110,0b01100110,0b01100110,0b00111110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'h': case 'H': {
            const uint8_t b[8] = {0b01100110,0b01100110,0b01100110,0b01111110,0b01100110,0b01100110,0b01100110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'i': case 'I': {
            const uint8_t b[8] = {0b00111100,0b00011000,0b00011000,0b00011000,0b00011000,0b00011000,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'j': case 'J': {
            const uint8_t b[8] = {0b00011110,0b00001100,0b00001100,0b00001100,0b00001100,0b01101100,0b00111000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'k': case 'K': {
            const uint8_t b[8] = {0b01100110,0b01101100,0b01111000,0b01110000,0b01111000,0b01101100,0b01100110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'l': case 'L': {
            const uint8_t b[8] = {0b01100000,0b01100000,0b01100000,0b01100000,0b01100000,0b01100000,0b01111110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'm': case 'M': {
            const uint8_t b[8] = {0b01100011,0b01110111,0b01111111,0b01101011,0b01100011,0b01100011,0b01100011,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'n': case 'N': {
            const uint8_t b[8] = {0b01100110,0b01110110,0b01111110,0b01111110,0b01101110,0b01100110,0b01100110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'o': case 'O': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100110,0b01100110,0b01100110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'p': case 'P': {
            const uint8_t b[8] = {0b01111100,0b01100110,0b01100110,0b01111100,0b01100000,0b01100000,0b01100000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'q': case 'Q': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100110,0b01100110,0b01101110,0b00111100,0b00000110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'r': case 'R': {
            const uint8_t b[8] = {0b01111100,0b01100110,0b01100110,0b01111100,0b01101100,0b01100110,0b01100110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 's': case 'S': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100000,0b00111100,0b00000110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 't': case 'T': {
            const uint8_t b[8] = {0b01111110,0b00011000,0b00011000,0b00011000,0b00011000,0b00011000,0b00011000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'u': case 'U': {
            const uint8_t b[8] = {0b01100110,0b01100110,0b01100110,0b01100110,0b01100110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'v': case 'V': {
            const uint8_t b[8] = {0b01100110,0b01100110,0b01100110,0b01100110,0b01100110,0b00111100,0b00011000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'w': case 'W': {
            const uint8_t b[8] = {0b01100011,0b01100011,0b01100011,0b01101011,0b01111111,0b01110111,0b01100011,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'x': case 'X': {
            const uint8_t b[8] = {0b01100110,0b01100110,0b00111100,0b00011000,0b00111100,0b01100110,0b01100110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'y': case 'Y': {
            const uint8_t b[8] = {0b01100110,0b01100110,0b01100110,0b00111100,0b00011000,0b00011000,0b00011000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case 'z': case 'Z': {
            const uint8_t b[8] = {0b01111110,0b00000110,0b00001100,0b00011000,0b00110000,0b01100000,0b01111110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '0': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01101110,0b01110110,0b01100110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '1': {
            const uint8_t b[8] = {0b00011000,0b00111000,0b00011000,0b00011000,0b00011000,0b00011000,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '2': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b00000110,0b00001100,0b00011000,0b00110000,0b01111110,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '3': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b00000110,0b00011100,0b00000110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '4': {
            const uint8_t b[8] = {0b00001100,0b00011000,0b00101100,0b01001100,0b01111110,0b00001100,0b00001100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '5': {
            const uint8_t b[8] = {0b01111110,0b01100000,0b01111100,0b00000110,0b00000110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '6': {
            const uint8_t b[8] = {0b00111100,0b01100000,0b01100000,0b01111100,0b01100110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '7': {
            const uint8_t b[8] = {0b01111110,0b00000110,0b00001100,0b00011000,0b00011000,0b00011000,0b00011000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '8': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100110,0b00111100,0b01100110,0b01100110,0b00111100,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '9': {
            const uint8_t b[8] = {0b00111100,0b01100110,0b01100110,0b00111110,0b00000110,0b00001100,0b00111000,0b00000000};
            memcpy(d, b, 8); break;
        }
        case '<': {
            const uint8_t b[8] = {0b00001100,0b00011000,0b00110000,0b01100000,0b00110000,0b00011000,0b00001100,0};
            memcpy(d, b, 8); break;
        }
        case '>': {
            const uint8_t b[8] = {0b00110000,0b00011000,0b00001100,0b00000110,0b00001100,0b00011000,0b00110000,0};
            memcpy(d, b, 8); break;
        }
        case '|': {
            const uint8_t b[8] = {0b00011000,0b00011000,0b00011000,0b00011000,0b00011000,0b00011000,0b00011000,0};
            memcpy(d, b, 8); break;
        }
        case '=': {
            const uint8_t b[8] = {0,0b01111110,0,0b01111110,0,0,0,0};
            memcpy(d, b, 8); break;
        }
        case '!': {
            const uint8_t b[8] = {0b00011000,0b00011000,0b00011000,0b00011000,0b00011000,0,0b00011000,0};
            memcpy(d, b, 8); break;
        }
        case '.': {
            const uint8_t b[8] = {0,0,0,0,0,0b00011000,0b00011000,0};
            memcpy(d, b, 8); break;
        }
        case ':': {
            const uint8_t b[8] = {0,0b00011000,0b00011000,0,0b00011000,0b00011000,0,0};
            memcpy(d, b, 8); break;
        }
        case '-': {
            const uint8_t b[8] = {0,0,0,0b01111110,0,0,0,0};
            memcpy(d, b, 8); break;
        }
        case '_': {
            const uint8_t b[8] = {0,0,0,0,0,0,0b01111110,0};
            memcpy(d, b, 8); break;
        }
        case '/': {
            const uint8_t b[8] = {0b00000010,0b00000100,0b00001000,0b00010000,0b00100000,0b01000000,0b10000000,0};
            memcpy(d, b, 8); break;
        }
        case '[': {
            const uint8_t b[8] = {0b00111100,0b00110000,0b00110000,0b00110000,0b00110000,0b00110000,0b00111100,0};
            memcpy(d, b, 8); break;
        }
        case ']': {
            const uint8_t b[8] = {0b00111100,0b00001100,0b00001100,0b00001100,0b00001100,0b00001100,0b00111100,0};
            memcpy(d, b, 8); break;
        }
        case '(': {
            const uint8_t b[8] = {0b00001100,0b00011000,0b00110000,0b00110000,0b00110000,0b00011000,0b00001100,0};
            memcpy(d, b, 8); break;
        }
        case ')': {
            const uint8_t b[8] = {0b00110000,0b00011000,0b00001100,0b00001100,0b00001100,0b00011000,0b00110000,0};
            memcpy(d, b, 8); break;
        }
        case ' ': {
            const uint8_t b[8] = {0,0,0,0,0,0,0,0};
            memcpy(d, b, 8); break;
        }
        default: {
            const uint8_t b[8] = {0,0,0,0,0,0,0,0};
            memcpy(d, b, 8); break;
        }
    }
}

void mk_ili9225_get_letter(uint16_t *fbuf, char l, uint16_t color, uint16_t bgcolor)
{
    uint8_t letter[8];
    mk_ili9225_get_raw_glyph(letter, l);
    for (uint8_t y = 0; y < 8; y++) {
        uint8_t row = letter[y];
        for (uint8_t x = 0; x < 8; x++) {
            fbuf[y * 8 + x] = (row & 128) ? color : bgcolor;
            row <<= 1;
        }
    }
}

void mk_ili9225_text(char *s, uint16_t x, uint16_t y, uint16_t color, uint16_t bgcolor)
{
    uint16_t fbuf[8 * 8];
    if (s == NULL) return;
    uint16_t cur_x = x;
    uint16_t cur_y = y;
    for (size_t i = 0; i < strlen(s); i++) {
        mk_ili9225_get_letter(fbuf, s[i], color, bgcolor);
        mk_ili9225_blit(fbuf, cur_x, cur_y, 8, 8);
        cur_x += 8;
        if (cur_x > (ILI9341_WIDTH - 8)) break;
    }
}

static inline uint16_t get_dimmed_pixel(const uint16_t *src_160x144, int px, int py, ili9341_scale_mode_t mode)
{
    (void)mode;
    if (src_160x144 == NULL) return 0x0000;
    if (px < 0 || px >= (int)ILI9341_WIDTH || py < 0 || py >= (int)ILI9341_HEIGHT) return 0x0000;

    int rx = (int)get_render_x();
    int ry = (int)get_render_y();
    int rw = (int)get_render_width();
    int rh = (int)get_render_height();

    int src_x = -1, src_y = -1;
    if (px >= rx && px < (rx + rw) && py >= ry && py < (ry + rh)) {
        src_x = ((px - rx) * (int)GB_LCD_WIDTH) / rw;
        src_y = ((py - ry) * (int)GB_LCD_HEIGHT) / rh;
    }

    if (src_x >= 0 && src_x < (int)GB_LCD_WIDTH && src_y >= 0 && src_y < (int)GB_LCD_HEIGHT) {
        uint16_t c = src_160x144[(size_t)src_y * GB_LCD_WIDTH + (size_t)src_x];
        /* 50% transparency black: shift R, G, B components down by 1 bit */
        return (uint16_t)((c & 0xF7DEu) >> 1);
    }
    return 0x0000;
}

void mk_ili9225_draw_dimmed_frame(const uint16_t *src_160x144)
{
    ili9341_wait_for_idle();
    ili9341_set_addr_window_raw(0, 0, ILI9341_WIDTH - 1u, ILI9341_HEIGHT - 1u);
    ili9341_select(true);
    ili9341_dc(true);

    static uint8_t scanline[ILI9341_WIDTH * 2u];
    for (int y = 0; y < (int)ILI9341_HEIGHT; y++) {
        for (int x = 0; x < (int)ILI9341_WIDTH; x++) {
            uint16_t c = get_dimmed_pixel(src_160x144, x, y, ili9341_scale_mode);
            scanline[x * 2]     = (uint8_t)(c >> 8);
            scanline[x * 2 + 1] = (uint8_t)(c & 0xFFu);
        }
        spi_write_blocking(spi0, scanline, sizeof(scanline));
    }

    ili9341_select(false);
}

void mk_ili9225_text_transparent(const char *s, uint16_t x, uint16_t y, uint16_t color, const uint16_t *src_160x144)
{
    if (s == NULL || x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;
    size_t len = strlen(s);
    if (len == 0) return;
    if (x + len * 8u > ILI9341_WIDTH) len = (ILI9341_WIDTH - x) / 8u;

    uint16_t w = (uint16_t)(len * 8u);
    uint16_t h = 8u;
    if (y + h > ILI9341_HEIGHT) h = ILI9341_HEIGHT - y;

    ili9341_wait_for_idle();
    ili9341_set_addr_window_raw(x, y, (uint16_t)(x + w - 1u), (uint16_t)(y + h - 1u));
    ili9341_select(true);
    ili9341_dc(true);

    static uint8_t line_tx[ILI9341_WIDTH * 2u];

    for (uint16_t row = 0; row < h; row++) {
        uint16_t cur_y = y + row;
        for (size_t i = 0; i < len; i++) {
            uint8_t glyph[8];
            mk_ili9225_get_raw_glyph(glyph, s[i]);
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                uint16_t cur_x = (uint16_t)(x + i * 8u + (size_t)col);
                uint16_t pixel;
                if (bits & (0x80 >> col)) {
                    pixel = color;
                } else {
                    pixel = get_dimmed_pixel(src_160x144, (int)cur_x, (int)cur_y, ili9341_scale_mode);
                }
                line_tx[(i * 8u + (size_t)col) * 2u]     = (uint8_t)(pixel >> 8);
                line_tx[(i * 8u + (size_t)col) * 2u + 1] = (uint8_t)(pixel & 0xFFu);
            }
        }
        spi_write_blocking(spi0, line_tx, w * 2u);
    }

    ili9341_select(false);
}

void mk_ili9341_render_volume_hud(void) {
    /* No-op: HUD is now stamped directly into band scanlines in mk_ili9225_write_pixels */
}
