/*
 * ili9341.c - Native pico-sdk SPI driver for 320x240 TFT displays (ILI9341 or
 * ST7789, selected at runtime).
 *
 * The driver keeps an internal 320x240 framebuffer in the native project
 * 16-bit pixel format (RGB444 packed as 0x0RGB). On flush, each pixel is
 * converted to RGB565 and sent to the panel via hardware SPI, optionally
 * using DMA so the CPU can keep rendering.
 *
 * Both controllers speak the same MIPI DCS framing and the same CASET/PASET/
 * RAMWR addressing, so one driver covers both; only the power-on sequence
 * differs (panel_init_sequence() in ili9341_init). The ST7789 branch reuses
 * the sequence already proven by PicoGBC's mk_st7789.c on this console.
 */

#include "ili9341.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"


// Framebuffer in the project's native 16-bit format.  Word-aligned because
// some conversion paths read it as uint32_t pairs.
static uint16_t __aligned(4) s_fb[ILI9341_WIDTH * ILI9341_HEIGHT];

// DMA channel used for SPI transfers.  Claimed once at init.
static int s_dma_chan = -1;
static volatile bool s_dma_busy = false;

// Scratch line: converted from native 16-bit to RGB565 before SPI send.
// One line keeps peak SRAM use low compared to a full second buffer.
static uint16_t __aligned(4) s_line[ILI9341_WIDTH];

// ILI9341 command constants
#define CMD_NOP         0x00
#define CMD_SWRESET     0x01
#define CMD_SLPIN       0x10
#define CMD_SLPOUT      0x11
#define CMD_PTLON       0x12
#define CMD_NORON       0x13
#define CMD_INVOFF      0x20
#define CMD_INVON       0x21
#define CMD_GAMSET      0x26
#define CMD_DISPOFF     0x28
#define CMD_DISPON      0x29
#define CMD_CASET       0x2A
#define CMD_PASET       0x2B
#define CMD_RAMWR       0x2C
#define CMD_MADCTL      0x36
#define CMD_COLMOD      0x3A
#define CMD_FRMCTR1     0xB1
#define CMD_FRMCTR2     0xB2
#define CMD_FRMCTR3     0xB3
#define CMD_INVCTR      0xB4
#define CMD_DFUNCTR     0xB6
#define CMD_PWCTR1      0xC0
#define CMD_PWCTR2      0xC1
#define CMD_PWCTR3      0xC2
#define CMD_PWCTR4      0xC3
#define CMD_PWCTR5      0xC4
#define CMD_VMCTR1      0xC5
#define CMD_GMCTRP1     0xE0
#define CMD_GMCTRN1     0xE1

static inline void cs_low(void)  { gpio_put(ILI9341_PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(ILI9341_PIN_CS, 1); }
static inline void dc_cmd(void)  { gpio_put(ILI9341_PIN_DC, 0); }
static inline void dc_data(void)  { gpio_put(ILI9341_PIN_DC, 1); }

// ---------------------------------------------------------------------------
// Panel state. All of this is set before ili9341_init() by the bootloader's
// pre-display hook (it reads /boot.txt, which needs the SD card mounted but not
// the panel), and the defaults below keep the plain ILI9341 build untouched.
// ---------------------------------------------------------------------------
static dmg_panel_t s_panel  = DMG_PANEL_ILI9341;
static bool        s_invert = false;
static uint8_t     s_xoff   = 0;
static uint8_t     s_yoff   = 0;
static uint8_t     s_madctl = 0x28;
static uint8_t     s_rotation = 1;      // 1 = landscape, the historical default
static bool        s_rotation_chosen = false;
// Set by ili9341_set_madctl() when the caller supplies a raw MADCTL value.
static bool        s_madctl_explicit = false;
static bool        s_inited = false;

// MADCTL for each rotation, per panel.
//
// ILI9341 keeps the values this driver has always used: 0x28 is the landscape
// default the project has shipped on, and it is correct for the PicoDMG
// module. Do not change these -- the ILI9341 build is the regression baseline.
//
// The ST7789 on the Waveshare Core2350B is a different bond, and the two axes
// had to be established separately:
//
//   - Mirror (MX): measured on hardware, 0x60 is 0x28 plus MX -- the same
//     landscape orientation with columns reversed, which is exactly the
//     mirroring that was reported.
//   - Colour order (BGR): the ST7789 takes the data as RGB when BGR is clear,
//     the opposite of the ILI9341's behaviour with the same bit. Inherited at
//     BGR=1 it swapped red and blue (1942's ocean came out red).
//
// BGR was never exercised while calibrating, because 0x28 and 0x60 differ only
// in MX: every candidate the on-screen calibrator offered shared one BGR
// setting. So the mirror was found by measurement and the colour order was
// never tested. Found with the on-screen calibrator rather than derived,
// because the mirror bits depend on how the controller is bonded to the glass
// and no datasheet states them.
#define ILI9341_MADCTL_LANDSCAPE   0x28
#define ILI9341_MADCTL_PORTRAIT    0x08
#define ST7789_MADCTL_LANDSCAPE    0x60
#define ST7789_MADCTL_PORTRAIT     0x40

static void select_madctl(void)
{
    const bool st7789 = (s_panel == DMG_PANEL_ST7789);
    // 180 degrees means flipping MY and MX, which is a toggle of both bits, not
    // a set of both. The distinction is invisible on the ILI9341 -- its
    // landscape value 0x28 has neither bit, so OR and XOR agree -- but on the
    // ST7789 it is the whole difference between 0xA0 (correct) and 0xE0 (the
    // ILI9341's inverted value, i.e. mirrored the wrong axis). Hence XOR, which
    // is correct for both panels.
    const uint8_t flip = 0xC0u;
    switch (s_rotation) {
        case 0:
            s_madctl = st7789 ? ST7789_MADCTL_PORTRAIT : ILI9341_MADCTL_PORTRAIT;
            break;
        case 1:
            s_madctl = st7789 ? ST7789_MADCTL_LANDSCAPE : ILI9341_MADCTL_LANDSCAPE;
            break;
        case 2:
            s_madctl = (uint8_t)((st7789 ? ST7789_MADCTL_PORTRAIT
                                         : ILI9341_MADCTL_PORTRAIT) ^ flip);
            break;
        case 3:
            s_madctl = (uint8_t)((st7789 ? ST7789_MADCTL_LANDSCAPE
                                         : ILI9341_MADCTL_LANDSCAPE) ^ flip);
            break;
    }
}

void ili9341_set_panel(dmg_panel_t panel)
{
    s_panel = panel;
}

dmg_panel_t ili9341_get_panel(void)
{
    return s_panel;
}

void ili9341_set_invert(bool on)
{
    s_invert = on;
}

bool ili9341_get_invert(void)
{
    return s_invert;
}

void ili9341_set_gram_offset(uint8_t xoff, uint8_t yoff)
{
    s_xoff = xoff;
    s_yoff = yoff;
}

void ili9341_get_gram_offset(uint8_t *xoff, uint8_t *yoff)
{
    if (xoff) *xoff = s_xoff;
    if (yoff) *yoff = s_yoff;
}

static void spi_write_byte(uint8_t b)
{
    spi_write_blocking(ILI9341_SPI, &b, 1);
}

static void write_cmd(uint8_t cmd)
{
    dc_cmd();
    cs_low();
    spi_write_byte(cmd);
    cs_high();
}

static void write_data(uint8_t data)
{
    dc_data();
    cs_low();
    spi_write_byte(data);
    cs_high();
}

static void write_data_buf(const uint8_t *buf, size_t len)
{
    dc_data();
    cs_low();
    spi_write_blocking(ILI9341_SPI, buf, len);
    cs_high();
}

static void set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    // GRAM offset. Both are 0 unless a panel needs otherwise, which makes this
    // a no-op for the ILI9341 and the default ST7789 wiring.
    x0 += s_xoff; x1 += s_xoff;
    y0 += s_yoff; y1 += s_yoff;

    write_cmd(CMD_CASET);
    buf[0] = (x0 >> 8) & 0xFF;
    buf[1] = x0 & 0xFF;
    buf[2] = (x1 >> 8) & 0xFF;
    buf[3] = x1 & 0xFF;
    write_data_buf(buf, 4);

    write_cmd(CMD_PASET);
    buf[0] = (y0 >> 8) & 0xFF;
    buf[1] = y0 & 0xFF;
    buf[2] = (y1 >> 8) & 0xFF;
    buf[3] = y1 & 0xFF;
    write_data_buf(buf, 4);
}

static void apply_madctl(void)
{
    write_cmd(CMD_MADCTL);
    write_data(s_madctl);
}

static void apply_invert(void)
{
    write_cmd(s_invert ? CMD_INVON : CMD_INVOFF);
}

// Paint the whole panel memory black. Neither the hardware reset nor SWRESET
// clears GRAM, and the power-switch standby only puts the panel to sleep
// (SLPIN) without cutting its power, so after a wake the panel still holds the
// last picker frame. Called right before DISPON so the screen lights up black
// instead of flashing that stale frame until the first real flush.
static void clear_gram(void)
{
    static const uint8_t zeros[2 * ILI9341_WIDTH] = { 0 };
    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);
    dc_data();
    cs_low();
    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        spi_write_blocking(ILI9341_SPI, zeros, sizeof(zeros));
    }
    cs_high();
}

// ST7789 power-on sequence, after the hardware reset. Deliberately the short
// one: it is byte-for-byte the sequence PicoGBC's mk_st7789.c runs in the field
// on this console (same CS/DC/RST/BL pins), so there is no reason to invent a
// longer variant here.
static void st7789_init_sequence(void)
{
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    apply_invert();

    write_cmd(CMD_COLMOD);
    write_data(0x55);  // 16-bit per pixel

    apply_madctl();

    // Panel refresh ~111 Hz instead of the 60 Hz default: the same PORCTRL /
    // FRCTRL2 values every emulator sends via dmg_panel_st7789_timing() in
    // shared_menu. A faster scan shortens the window in which a write can be
    // caught half-done (tearing in the intro and menus). ST7789-only commands:
    // they must not go to the ILI9341.
    {
        static const uint8_t porctrl[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
        static const uint8_t frctrl2[] = { 0x01 };   // 0x0F = 60 Hz default
        write_cmd(0xB2);
        write_data_buf(porctrl, sizeof(porctrl));
        write_cmd(0xC6);
        write_data_buf(frctrl2, sizeof(frctrl2));
    }

    clear_gram();

    write_cmd(CMD_NORON);
    sleep_ms(10);
    write_cmd(CMD_DISPON);
    sleep_ms(120);
}

// ILI9341 power-on sequence, after the hardware reset. This is the recipe the
// console has always run, moved out of ili9341_init() so the panel switch is a
// plain choice between two functions. The only behaviour change is that MADCTL
// now comes from s_madctl (0x28 unless ROTATE=1) and the panel is finally told
// INVOFF explicitly rather than never being told anything.
static void ili9341_init_sequence(void)
{
    // Initialisation sequence (common ILI9341 recipe)
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(CMD_PWCTR1);
    write_data(0x23);
    write_data(0x00);

    write_cmd(CMD_PWCTR2);
    write_data(0x10);

    write_cmd(CMD_VMCTR1);
    write_data(0x3E);
    write_data(0x28);

    write_cmd(CMD_VMCTR1);
    write_data(0x86);

    apply_madctl();  // 0x28 landscape, RGB order, row/col exchange

    write_cmd(CMD_COLMOD);
    write_data(0x55);  // 16-bit per pixel

    write_cmd(CMD_FRMCTR1);
    write_data(0x00);
    write_data(0x18);

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

    write_cmd(CMD_VMCTR1);
    write_data(0x0E);

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

    clear_gram();

    write_cmd(CMD_DISPON);
    sleep_ms(120);

    apply_invert();
}

void ili9341_init(void)
{
    // Configure GPIOs
    gpio_init(ILI9341_PIN_CS);
    gpio_set_dir(ILI9341_PIN_CS, GPIO_OUT);
    gpio_put(ILI9341_PIN_CS, 1);

    gpio_init(ILI9341_PIN_DC);
    gpio_set_dir(ILI9341_PIN_DC, GPIO_OUT);
    gpio_put(ILI9341_PIN_DC, 1);

    gpio_init(ILI9341_PIN_RST);
    gpio_set_dir(ILI9341_PIN_RST, GPIO_OUT);
    gpio_put(ILI9341_PIN_RST, 1);

    gpio_init(ILI9341_PIN_BL);
    gpio_set_dir(ILI9341_PIN_BL, GPIO_OUT);
    gpio_put(ILI9341_PIN_BL, 0);  // off until explicitly enabled

    // Configure SPI at a conservative 30 MHz init speed; bumped later.
    spi_init(ILI9341_SPI, 30 * 1000 * 1000);
    gpio_set_function(ILI9341_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(ILI9341_PIN_SCK, GPIO_FUNC_SPI);
    // MISO is not needed for the display; leave it as GPIO if defined elsewhere.

    // Hardware reset. Both panels need it before their command sequence.
    gpio_put(ILI9341_PIN_RST, 0);
    sleep_ms(10);
    gpio_put(ILI9341_PIN_RST, 1);
    sleep_ms(120);

    // Panel choice comes from /boot.txt (ST7789=1) via Frens_preDisplayHook(),
    // which runs between initSDCard() and this function. The ILI9341 branch is
    // the pre-existing behaviour and stays the default.
    if (s_panel == DMG_PANEL_ST7789) {
        st7789_init_sequence();
    } else {
        ili9341_init_sequence();
    }

    // Bump SPI to the fastest rate this panel reliably supports on Pico.
    spi_set_baudrate(ILI9341_SPI, 62 * 1000 * 1000);

    // Claim a DMA channel for asynchronous frame transfers.
    s_dma_chan = dma_claim_unused_channel(true);

    // Clear framebuffer to black.
    memset(s_fb, 0, sizeof(s_fb));

    s_inited = true;
}

void ili9341_set_backlight(bool on)
{
    gpio_put(ILI9341_PIN_BL, on ? 1 : 0);
}

void ili9341_set_rotation(uint8_t rotation)
{
    s_rotation = rotation & 3;
    s_rotation_chosen = true;
    // An explicit MADCTL (boot.txt MADCTL=) is a deliberate override of the
    // rotation-derived value, so a later set_rotation() must not quietly undo
    // it. Callers that only ever use rotation are unaffected.
    if (!s_madctl_explicit) {
        select_madctl();
    }
    // Apply immediately if the panel is already up, otherwise leave it to the
    // power-on sequence.
    if (s_inited) {
        apply_madctl();
    }
}

void ili9341_set_madctl(uint8_t madctl)
{
    s_madctl = madctl;
    s_madctl_explicit = true;
    // Counts as "chosen" for the same reason set_rotation() does: initDVandAudio()
    // calls apply_default_rotation() later and must not re-derive over the top.
    s_rotation_chosen = true;
    if (s_inited) {
        apply_madctl();
    }
}

uint8_t ili9341_get_madctl(void)
{
    return s_madctl;
}

uint8_t ili9341_apply_default_rotation(uint8_t rotation)
{
    // A rotation chosen before power-on (the bootloader's /boot.txt ROTATE
    // key, applied from the pre-display hook) always wins over this default.
    if (!s_rotation_chosen) {
        s_rotation = rotation & 3;
        select_madctl();
    }
    if (s_inited) {
        apply_madctl();
    }
    return s_rotation;
}

uint16_t *ili9341_get_framebuffer(void)
{
    return s_fb;
}

// Convert the project's native 16-bit pixel (RGB444 0x0RGB) to ILI9341 RGB565.
// A lookup table would be faster, but this inline path is small and keeps
// memory use predictable for the bootloader.
static inline uint16_t rgb444_to_rgb565(uint16_t c)
{
    uint8_t r = (c >> 8) & 0x0F;
    uint8_t g = (c >> 4) & 0x0F;
    uint8_t b = c & 0x0F;
    uint8_t r5 = (r << 1) | (r >> 3);
    uint8_t g6 = (g << 2) | (g >> 2);
    uint8_t b5 = (b << 1) | (b >> 3);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static void send_line_dma(const uint16_t *line, size_t n)
{
    dma_channel_config cfg = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, spi_get_dreq(ILI9341_SPI, true));

    dma_channel_configure(s_dma_chan, &cfg,
                          &spi_get_hw(ILI9341_SPI)->dr,
                          line, n, true);
}

static bool s_gb_mode = false;

void ili9341_set_gb_mode(bool enabled)
{
    s_gb_mode = enabled;
}

bool ili9341_get_gb_mode(void)
{
    return s_gb_mode;
}

// When set, s_fb already holds RGB565 (graphical-menu artwork from .565
// files) and the flush paths send it unchanged instead of expanding RGB444.
static bool s_native_565 = false;

void ili9341_set_native_565(bool enabled)
{
    s_native_565 = enabled;
}

bool ili9341_get_native_565(void)
{
    return s_native_565;
}

static inline uint16_t fb_to_565(uint16_t c)
{
    return s_native_565 ? c : rgb444_to_rgb565(c);
}

void ili9341_flush(bool wait)
{
    if (s_dma_chan < 0) return;

    // Wait for any previous async flush to finish.
    if (s_dma_busy) {
        dma_channel_wait_for_finish_blocking(s_dma_chan);
        s_dma_busy = false;
    }

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();

    // Switch SPI controller to 16-bit data format so that 16-bit RGB565 pixels
    // are transmitted with all 16 bits over SPI (instead of 8-bit truncation).
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        const uint16_t *src = &s_fb[y * ILI9341_WIDTH];
        if (s_gb_mode) {
            for (int x = 0; x < 45; x++) {
                s_line[x] = 0x0000;
            }
            for (int x = 45; x <= 310; x++) {
                s_line[x] = fb_to_565(src[x]);
            }
            for (int x = 311; x < ILI9341_WIDTH; x++) {
                s_line[x] = 0x0000;
            }
        } else {
            for (int x = 0; x < ILI9341_WIDTH; x++) {
                s_line[x] = fb_to_565(src[x]);
            }
        }
        send_line_dma(s_line, ILI9341_WIDTH);
        dma_channel_wait_for_finish_blocking(s_dma_chan);
    }

    // Wait for the SPI shift register and TX FIFO to fully drain
    while (spi_is_busy(ILI9341_SPI)) {
        tight_loop_contents();
    }
    // Drain any leftover data in RX FIFO
    while (spi_is_readable(ILI9341_SPI)) {
        (void)spi_get_hw(ILI9341_SPI)->dr;
    }

    cs_high();

    // Restore SPI controller to 8-bit data format for command sequences
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void ili9341_flush_rows(int y_start, int y_end)
{
    if (s_dma_chan < 0) return;
    if (y_start < 0) y_start = 0;
    if (y_end >= ILI9341_HEIGHT) y_end = ILI9341_HEIGHT - 1;
    if (y_start > y_end) return;

    if (s_dma_busy) {
        dma_channel_wait_for_finish_blocking(s_dma_chan);
        s_dma_busy = false;
    }

    set_addr_window(0, y_start, ILI9341_WIDTH - 1, y_end);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();

    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    for (int y = y_start; y <= y_end; y++) {
        const uint16_t *src = &s_fb[y * ILI9341_WIDTH];
        if (s_gb_mode) {
            for (int x = 0; x < 45; x++) {
                s_line[x] = 0x0000;
            }
            for (int x = 45; x <= 310; x++) {
                s_line[x] = fb_to_565(src[x]);
            }
            for (int x = 311; x < ILI9341_WIDTH; x++) {
                s_line[x] = 0x0000;
            }
        } else {
            for (int x = 0; x < ILI9341_WIDTH; x++) {
                s_line[x] = fb_to_565(src[x]);
            }
        }
        send_line_dma(s_line, ILI9341_WIDTH);
        dma_channel_wait_for_finish_blocking(s_dma_chan);
    }

    while (spi_is_busy(ILI9341_SPI)) {
        tight_loop_contents();
    }
    while (spi_is_readable(ILI9341_SPI)) {
        (void)spi_get_hw(ILI9341_SPI)->dr;
    }

    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void ili9341_flush_async(void)
{
    // Start a full-frame flush.  The caller must call ili9341_flush_busy()
    // and wait before starting another flush.
    if (s_dma_chan < 0) return;

    if (s_dma_busy) {
        dma_channel_wait_for_finish_blocking(s_dma_chan);
        s_dma_busy = false;
    }

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();

    // Convert first line and start DMA.
    const uint16_t *src = s_fb;
    for (int x = 0; x < ILI9341_WIDTH; x++) {
        s_line[x] = fb_to_565(src[x]);
    }
    send_line_dma(s_line, ILI9341_WIDTH);
    s_dma_busy = true;
}

bool ili9341_flush_busy(void)
{
    if (!s_dma_busy) return false;
    if (dma_channel_is_busy(s_dma_chan)) return true;
    s_dma_busy = false;
    cs_high();
    return false;
}

void ili9341_write_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= ILI9341_WIDTH || y < 0 || y >= ILI9341_HEIGHT) return;
    s_fb[y * ILI9341_WIDTH + x] = color;
}

void ili9341_fill_rect(int x, int y, int w, int h, uint16_t color)
{
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

void ili9341_fill_screen(uint16_t color)
{
    ili9341_fill_rect(0, 0, ILI9341_WIDTH, ILI9341_HEIGHT, color);
}

// ---------------------------------------------------------------------------
// Horizontal hardware scroll (graphical-menu slide).
//
// Both controllers scroll along the gate direction, which is the 320-pixel
// axis -- in landscape (MADCTL MV set) that is the screen's X axis. So a slide
// only has to update VSCRSADD and write the newly revealed columns, instead of
// re-sending the whole 153 KB frame every step. The old image stays put in
// GRAM; the panel just starts reading it at another line.
//
// With MV set, screen X maps to frame-memory row X when MY is clear (0x28,
// 0x60) and to row 319-X when MY is set (ROTATE=1: 0xE8, 0xA0), which flips
// both which fixed area is which and the sign of the scroll offset.
// ---------------------------------------------------------------------------
#define CMD_VSCRDEF  0x33
#define CMD_VSCRSADD 0x37

static bool s_hs_active = false;
static bool s_hs_rev    = false;
static int  s_hs_w      = ILI9341_WIDTH;
static int  s_hs_tfa    = 0;

static void write_u16_params(const uint16_t *v, int n)
{
    uint8_t buf[6];
    for (int i = 0; i < n; i++) {
        buf[2 * i]     = (uint8_t)(v[i] >> 8);
        buf[2 * i + 1] = (uint8_t)(v[i] & 0xFF);
    }
    write_data_buf(buf, (size_t)(2 * n));
}

bool ili9341_hscroll_begin(int x, int w)
{
    if (s_dma_chan < 0) return false;
    if (!(s_madctl & 0x20)) return false;          // portrait: scroll axis is Y
    if (s_xoff || s_yoff) return false;            // offset panels: not mapped
    if (x < 0 || w < 2 || x + w > ILI9341_WIDTH) return false;

    s_hs_rev = (s_madctl & 0x80) != 0;
    s_hs_w   = w;
    s_hs_tfa = s_hs_rev ? (ILI9341_WIDTH - (x + w)) : x;
    const uint16_t def[3] = { (uint16_t)s_hs_tfa, (uint16_t)w,
                              (uint16_t)(ILI9341_WIDTH - s_hs_tfa - w) };
    write_cmd(CMD_VSCRDEF);
    write_u16_params(def, 3);
    s_hs_active = true;
    ili9341_hscroll_set(0);
    return true;
}

void ili9341_hscroll_set(int p)
{
    if (!s_hs_active) return;
    p %= s_hs_w;
    if (p < 0) p += s_hs_w;
    const int v = s_hs_rev ? (s_hs_w - p) % s_hs_w : p;
    const uint16_t addr = (uint16_t)(s_hs_tfa + v);
    write_cmd(CMD_VSCRSADD);
    write_u16_params(&addr, 1);
}

void ili9341_hscroll_end(void)
{
    if (!s_hs_active) return;
    const uint16_t zero = 0;
    write_cmd(CMD_VSCRSADD);
    write_u16_params(&zero, 1);
    const uint16_t def[3] = { 0, ILI9341_WIDTH, 0 };
    write_cmd(CMD_VSCRDEF);
    write_u16_params(def, 3);
    write_cmd(CMD_NORON);                          // leave scroll mode
    s_hs_active = false;
}

// Second line buffer so the next row is composed while the DMA sends this one.
static uint16_t __aligned(4) s_line2[ILI9341_WIDTH];

void ili9341_write_cols_565(int x0, int x1, ili9341_row_fn row, void *ctx)
{
    if (s_dma_chan < 0 || !row) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= ILI9341_WIDTH) x1 = ILI9341_WIDTH - 1;
    if (x0 > x1) return;
    const int n = x1 - x0 + 1;

    if (s_dma_busy) {
        dma_channel_wait_for_finish_blocking(s_dma_chan);
        s_dma_busy = false;
    }

    set_addr_window((uint16_t)x0, 0, (uint16_t)x1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);
    dc_data();
    cs_low();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    uint16_t *bufs[2] = { s_line, s_line2 };
    row(0, bufs[0], x0, n, ctx);
    for (int y = 0; y < ILI9341_HEIGHT; y++) {
        uint16_t *cur = bufs[y & 1];
        if (y > 0) dma_channel_wait_for_finish_blocking(s_dma_chan);
        send_line_dma(cur, (size_t)n);
        if (y + 1 < ILI9341_HEIGHT) row(y + 1, bufs[(y + 1) & 1], x0, n, ctx);
    }
    dma_channel_wait_for_finish_blocking(s_dma_chan);

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;
    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

// Write the RGB565 rectangle [x0..x1] x [y0..y1] of a 320-wide buffer to the
// panel one screen column at a time, left to right.
//
// In landscape the panel scans along screen X (its 320-pixel gate axis), so a
// normal row-by-row write gets crossed by the scan along a diagonal: part of
// every row is already new, the rest still old -- a sheared, torn image while
// something moves. Writing in column order, the scan can only meet the write
// front once, which leaves at most one clean vertical seam. Each column is its
// own small window, which keeps the mapping identical to every other write
// (no MADCTL games) at a cost of 11 command bytes per column.
void ili9341_write_rect_by_columns(const uint16_t *fb, int x0, int x1, int y0, int y1)
{
    if (!fb) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= ILI9341_WIDTH)  x1 = ILI9341_WIDTH - 1;
    if (y1 >= ILI9341_HEIGHT) y1 = ILI9341_HEIGHT - 1;
    if (x0 > x1 || y0 > y1) return;

    if (s_dma_busy) {
        dma_channel_wait_for_finish_blocking(s_dma_chan);
        s_dma_busy = false;
    }

    uint8_t col[2 * ILI9341_HEIGHT];
    const int h = y1 - y0 + 1;
    for (int x = x0; x <= x1; x++) {
        for (int i = 0; i < h; i++) {
            const uint16_t c = fb_to_565(fb[(y0 + i) * ILI9341_WIDTH + x]);
            col[2 * i]     = (uint8_t)(c >> 8);
            col[2 * i + 1] = (uint8_t)(c & 0xFF);
        }
        set_addr_window((uint16_t)x, (uint16_t)y0, (uint16_t)x, (uint16_t)y1);
        write_cmd(CMD_RAMWR);
        write_data_buf(col, (size_t)(2 * h));
    }
}
