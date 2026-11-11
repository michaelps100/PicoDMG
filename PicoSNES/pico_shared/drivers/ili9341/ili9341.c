#include "ili9341.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"

#include "font_8x8.h"

/* shared_menu: runtime panel selection (MADCTL + orientation), the ST7789
 * refresh overclock, GB_MODE window geometry and the volume HUD rows. */
#include "dmg_panel.h"
#include "dmg_menu.h"

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

/* Requested run baud for the panel.
 *
 * IMPORTANT: this is a REQUEST, not the rate you get. The RP2350 PL022 divides
 * clk_peri by (prescale x postdiv) where prescale must be EVEN and postdiv is an
 * integer. clk_peri is slaved 1:1 to clk_sys, which runs at 360 MHz, so the
 * divisors on offer are 360/2=180, /4=90, /6=60, /8=45 MHz ... 72 MHz is not on
 * that grid: 360/72 = 5, and 5 is odd.
 *
 * The SDK therefore settles on prescale=2, postdiv=3 and the panel actually runs
 * at 60 MHz. That is a 20% shortfall against this comment's intent, and it
 * matters: the 111 Hz refresh overclock below exists to separate a ~14 ms SPI
 * write from the ~16.6 ms factory glass sweep, and at 60 MHz a full 320x240 panel
 * write is ~20.5 ms. The sweep finishes early but the write does not, so the
 * tearing the overclock was added to fix comes back.
 *
 * Reaching a true 72 MHz needs clk_peri decoupled from clk_sys (288 MHz gives
 * 288/2/2; 144 MHz gives 144/2). On this part that means routing clk_peri to
 * PLL_USB, whose programming API differs from RP2040's -- see agents.md. Until
 * that is done, read the real rate off the boot line, which prints
 * spi_get_baudrate() rather than this constant.
 *
 * spi_get_baudrate() is what ili9341_init() prints. */
#define SPI_RUN_BAUD   72000000u

/* Power-on sequences, chosen by panel at runtime. Forward declared here so the
 * dispatch in ili9341_init() reads top-down. */
static void ili9341_init_sequence(void);
static void st7789_init_sequence(void);

#if !defined(ILI9341_EXTERNAL_FB)
/* Framebuffer used by the menu system (320x240 RGB555) */
static uint16_t __aligned(4) s_fb_static[ILI9341_WIDTH * ILI9341_HEIGHT];
static uint16_t *s_fb = s_fb_static;
#else
static uint16_t *s_fb = NULL;
#endif

void ili9341_set_framebuffer(uint16_t *fb) {
    s_fb = fb;
}

/* ------------------------------------------------------------------ */
/* Per-scanline streaming blitter state (Dual-Core PicoDMG strategy)   */
/* ------------------------------------------------------------------ */
/* 32 slots = 20 KB. It was 64 (40 KB); the extra 20 KB now pays for the Super FX
 * core in SRAM. One 16-row strip pushes at most ~18 rows, so 32 still holds a
 * whole strip while Core 1 drains, and the column caging made each row 15-20%
 * cheaper on SPI. ili9341_core0_wait_us() (reported in perf.txt) is the check
 * that this depth is enough -- raise it again only if that number says so. */
#define NES_LCD_SLOTS 32u
#define NES_LCD_SLOTS_MASK (NES_LCD_SLOTS - 1u)
#define NES_ROW_BYTES (ILI9341_WIDTH * 2u)

static uint8_t s_slot_row[NES_LCD_SLOTS];
/* Active column window of each slot. The slot buffer keeps the full 320-px
 * layout; Core 1 sends only [x0, x0 + w) of it. Black border columns are painted
 * once (ili9341_clear_black / a full-width row) and then left alone, which cuts
 * SPI traffic by the border share: Fit 274/320, GB window 266/320, Integer
 * 256/320. In Progressive a full 320x240 frame at 70 MHz is ~17.5 ms -- over the
 * 16.64 ms budget -- so the border bytes were what kept it under 60 fps. */
static uint16_t s_slot_x0[NES_LCD_SLOTS];
static uint16_t s_slot_w[NES_LCD_SLOTS];
static volatile uint8_t s_slot_state[NES_LCD_SLOTS]; /* 1 = ready for Core 1 */
static volatile uint8_t s_write_slot = 0u;
static volatile uint8_t s_read_slot = 0u;
static volatile bool s_blitter_core1_launched = false;
static volatile bool s_blitter_active = false;

/* The slot payload. Core 0 fills one, hands it over with s_slot_state, Core 1
 * drains it with ili9341_stream_row().
 *
 * This MUST be a 2D array, as it is in every other emulator in this project
 * (PicoNGP, PicoMD): `static uint8_t __aligned(4) s_slot_data[NES_LCD_SLOTS][NES_ROW_BYTES];`
 *
 * It was here declared as `uint8_t *s_slot_data[NES_LCD_SLOTS]` -- 8 pointers and
 * no storage at all, which is a stub someone added to make the uses compile. Every
 * write then went to NULL + offset, i.e. into the XIP flash window at
 * 0x00000000..0x0000027F, which is read-only, and Core 1 read that flash back as
 * if it were pixels.
 *
 * That is why the whole diagnostic suite looked healthy while the screen stayed
 * black, and it is why the two disagree by 24x on the same frame:
 *
 *   port  : 74 non-zero pixels per row  (the real strip, measured before scaling)
 *   Core 1: 313 non-zero pixels per row (bits read out of flash, not pixels)
 *
 * 313/320 is also above what GB_MODE can produce at all -- the window is 266 px of
 * 320, so at most 83% of a row can be image. A row cannot be 98% non-black.
 *
 * Cost: 8 * 640 = 5120 bytes of BSS. RAM was 48.22% used, so this is affordable.
 *
 * A 2D array is a drop-in replacement for the pointer version: `s_slot_data[slot]`
 * decays to `uint8_t *` in every use below, which is why the call sites did not
 * have to change when this went wrong. */
static uint8_t __aligned(4) s_slot_data[NES_LCD_SLOTS][NES_ROW_BYTES];

static inline void slot_set_full_width(uint8_t slot) {
    s_slot_x0[slot] = 0u;
    s_slot_w[slot] = ILI9341_WIDTH;
}

/* Frames during which the SNES path sends every row full width, so that border
 * columns left over from a previous geometry (or the FPS digits) are repainted
 * black. Two frames so that both interleave fields are covered. */
static volatile uint8_t s_full_width_frames = 0u;
static bool s_rows_pushed_this_frame = false;

/* Microseconds Core 0 spent blocked on a full slot queue (SNES path). */
static uint32_t s_core0_wait_us = 0u;
uint32_t ili9341_core0_wait_us(void) { return s_core0_wait_us; }
void ili9341_core0_wait_reset(void) { s_core0_wait_us = 0u; }

static volatile bool s_scanlines_enabled = false;
static bool s_md_interleave = true;
static ili9341_scale_mode_t s_scale_mode = SCALE_FIT;

static uint8_t s_next_row = 0u;
static uint8_t s_field_parity = 0u; /* 0 or 1: interlaced fields to halve SPI bandwidth */
static uint32_t s_row_hash[ILI9341_HEIGHT];
static bool s_bov_frame_rows = false;   /* low-battery overlay: rows emitted this frame */

/* Core 1 -> panel boundary counters (see nes_blitter_core1()).
 *
 * Every other diagnostic in this port sits upstream of this point and therefore
 * cannot tell a working core from a broken display path. ili9341_frame_nonzero()
 * counts in ili9341_stream_snes_line() *before* snes_band_push(), so it is happy
 * while the panel stays black -- which is exactly what it did for many revisions.
 * These three count rows that came out of the slot queue and were handed to the
 * panel, which is the last boundary before the SPI write.
 *
 * s_c1_row_changes guards against the silent failure this replaces: if the
 * blitter were dumping one row over and over (or the core were publishing a
 * constant row index) the row count alone would look healthy. */
static volatile uint32_t s_c1_rows = 0;
static volatile uint32_t s_c1_nz = 0;
static volatile uint32_t s_c1_row_changes = 0;
static volatile uint8_t s_c1_last_row = 0xFF;

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

/* Adapter for the shared panel timing helper.
 *
 * dmg_panel_st7789_timing() takes one combined callback (command + parameter
 * block). This driver keeps write_cmd()/write_data() as separate primitives
 * because the Core 1 streaming path interleaves them, so the combined shape is
 * bridged here in four lines rather than by changing the whole driver. */
static void panel_timing_write(uint8_t cmd, const uint8_t *data, size_t len) {
    write_cmd(cmd);
    if (data != NULL && len > 0) {
        write_data_buf(data, len);
    }
}

void ili9341_set_rotation(uint8_t rotation) {
    /* The effective MADCTL comes from shared_menu, not from this file.
     *
     * The panel is a runtime choice (ST7789= in /boot.txt), and the two
     * controllers do not agree on the same bits: the ST7789 on the Core2350B
     * needs 0x60 in landscape with BGR cleared, the ILI9341 needs 0x28 with
     * BGR set. The table and the XOR-vs-OR rule for 180 degrees live in
     * dmg_panel.h; see there for why those values are measured rather than
     * derived.
     *
     * An explicit rotation call still overrides /boot.txt, so it maps the four
     * rotation ids onto the panel's own pair of values before XOR-ing 0xC0 for
     * the inverted orientations. */
    const uint8_t portrait  = (dmg_panel_get() == DMG_PANEL_ST7789)
                                ? DMG_MADCTL_ST7789_PORTRAIT
                                : DMG_MADCTL_ILI9341_PORTRAIT;
    const uint8_t landscape = (dmg_panel_get() == DMG_PANEL_ST7789)
                                ? DMG_MADCTL_ST7789_LANDSCAPE
                                : DMG_MADCTL_ILI9341_LANDSCAPE;

    uint8_t madctl;
    switch (rotation & 3) {
        case DMG_ROT_PORTRAIT:      madctl = portrait;  break;
        case DMG_ROT_LANDSCAPE:     madctl = landscape; break;
        /* XOR 0xC0, not OR 0xC0. They agree on the ILI9341, whose landscape
         * value sets neither MY nor MX, which is exactly why the difference is
         * easy to miss. On the ST7789, 0x60 already has MX set, so OR would only
         * add MY and return the ILI9341's inverted value -- the same image
         * mirrored on the wrong axis. */
        case DMG_ROT_PORTRAIT_INV:  madctl = (uint8_t)(portrait  ^ 0xC0u); break;
        case DMG_ROT_LANDSCAPE_INV: madctl = (uint8_t)(landscape ^ 0xC0u); break;
        default:                    madctl = landscape; break;
    }
    write_cmd(CMD_MADCTL);
    write_data(madctl);
}

/* Write the MADCTL the shared panel module resolved, honouring an explicit
 * MADCTL= in /boot.txt. Used by the power-on sequences, which run before any
 * rotation call. */
static void panel_apply_madctl(void) {
    write_cmd(CMD_MADCTL);
    write_data(dmg_panel_get_madctl());
}

/* Converts 15-bit RGB555 to ILI9341 16-bit RGB565 via fast bit manipulation */
static inline uint16_t rgb555_to_rgb565(uint16_t c) {
    return (uint16_t)(((c & 0x7FE0u) << 1) | ((c >> 4) & 0x0020u) | (c & 0x001Fu));
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

    /* The two controllers share almost nothing after the reset. Sending the
     * wrong power-on is not an error -- an unassigned command is silently
     * ignored -- so the sequence that leaves a panel looking correct is easy to
     * confuse with one that was meant for it. Hence the split rather than a
     * runtime switch inside one sequence. */
    if (dmg_panel_get() == DMG_PANEL_ST7789) {
        st7789_init_sequence();
    } else {
        ili9341_init_sequence();
    }

    /* Switch SPI to operational baud rate */
    spi_set_baudrate(ILI9341_SPI, SPI_RUN_BAUD);

    nes_prepare_band_xmap();
    if (s_fb) memset(s_fb, 0, ILI9341_WIDTH * ILI9341_HEIGHT * sizeof(uint16_t));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    if (dmg_panel_get() == DMG_PANEL_ST7789) {
        printf("ST7789: initialized %ux%u, MADCTL=0x%02x (%s) @ %u Hz\n",
               (unsigned)ILI9341_WIDTH, (unsigned)ILI9341_HEIGHT,
               (unsigned)dmg_panel_get_madctl(), dmg_panel_source(),
               spi_get_baudrate(ILI9341_SPI));
    } else {
        printf("ILI9341: initialized in landscape 320x240 @ %u Hz (Interlaced Core 1 streaming)\n",
               spi_get_baudrate(ILI9341_SPI));
    }
}

/* ILI9341 power-on. Assumes pins, SPI and both resets are already done. */
static void ili9341_init_sequence(void) {
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

    panel_apply_madctl();

    write_cmd(CMD_COLMOD);
    write_data(0x55); /* 16-bit RGB565 */

    /* FRMCTR1 is ILI9341-only. It must not be sent to an ST7789, and equally
     * the ST7789's 0xB2/0xC6 below must not be sent here: both directions are
     * no-ops on the wrong controller, which is why a mix-up produces a panel
     * that boots and looks fine and simply never gets the effect.
     *
     * 0x18 is the value eight of this project's emulators ship on the RP2350
     * Plus and is the effectively validated path for that glass. It is NOT
     * centralised in dmg_panel.c yet, because the alternative candidates
     * (0x10, ~100 Hz) have never been measured on this hardware and choosing
     * between them needs a scope, not a preference. See root AGENTS.md sec. 26. */
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

/* ST7789 power-on for the Waveshare Core2350B panel.
 *
 * This is the same sequence PicoGBC/mk_st7789.c has been running on this real
 * glass, which is why it is transcribed rather than re-derived: it is the one
 * validated on the hardware. The refresh overclock itself is NOT re-declared
 * here -- it goes through dmg_panel_st7789_timing() so all ten emulators share
 * one copy of those values. */
static void st7789_init_sequence(void) {
    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    if (dmg_panel_get_invert()) {
        write_cmd(CMD_INVON);
    } else {
        write_cmd(CMD_INVOFF);
    }

    write_cmd(CMD_COLMOD);
    write_data(0x55); /* 16-bit RGB565 */

    panel_apply_madctl();

    /* Physical panel timing: PORCTRL (0xB2) + FRCTRL2 (0xC6 = 0x01, 111 Hz).
     *
     * The panel sweeps its own glass at a fixed rate the emulator does not
     * control: the factory ST7789 sweep is ~16.6 ms and one frame takes Core 1
     * ~14 ms at 72 MHz, so the GRAM write and the beam scan overlap, and the
     * overlap shows as a horizontal tear through fades and cutscenes. 111 Hz
     * (~9 ms) separates them. It belongs here, after MADCTL and before the
     * window, and never in the ILI9341 branch above. */
    dmg_panel_st7789_timing(panel_timing_write);

    uint8_t xoff = 0, yoff = 0;
    dmg_panel_get_gram_offset(&xoff, &yoff);
    set_addr_window((uint16_t)xoff, (uint16_t)yoff,
                    (uint16_t)(ILI9341_WIDTH - 1u + xoff),
                    (uint16_t)(ILI9341_HEIGHT - 1u + yoff));

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

    if (s_fb) memset(s_fb, 0, ILI9341_WIDTH * ILI9341_HEIGHT * sizeof(uint16_t));
}

static volatile bool s_fps_enabled = false;
static char s_fps_text[16] = "60";

void ili9341_set_fps_overlay(bool enable, const char *fps_str) {
    if (s_fps_enabled != enable) {
        s_fps_enabled = enable;
        for (int r = 8; r < 16; r++) {
            s_row_hash[r] = 0u;
        }
        /* Turning the overlay off: its digits may sit in a border column that
         * caged rows no longer send, so rows 8..15 go out full width once more. */
        s_full_width_frames = 2u;
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

/* Sends columns [x0, x0 + w) of a single scanline in a single transaction.
 * Still one CS/PASET/RAMWR per row: batching rows into one window is what
 * produced tearing on the 111 Hz ST7789 (agents.md sec. 56). */
static void __not_in_flash_func(ili9341_stream_row)(const uint8_t *packed, uint16_t row,
                                                    uint16_t x0, uint16_t w) {
    if (packed == NULL || row >= ILI9341_HEIGHT) return;
    if (w == 0u || x0 >= ILI9341_WIDTH) return;
    if (x0 + w > ILI9341_WIDTH) w = (uint16_t)(ILI9341_WIDTH - x0);
    const uint16_t x1 = (uint16_t)(x0 + w - 1u);

    cs_low();

    /* 0. Set column address (CASET) */
    dc_cmd();
    uint8_t cmd_caset = CMD_CASET;
    spi_write_blocking(ILI9341_SPI, &cmd_caset, 1);

    dc_data();
    uint8_t caset[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    spi_write_blocking(ILI9341_SPI, caset, 4);

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
    spi_write_blocking(ILI9341_SPI, packed + ((size_t)x0 << 1), (size_t)w << 1);
    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();

    cs_high();
}

/* Core 1 background display worker.
 *
 * Core 1 does the SPI transfer and nothing else: it takes an already-scaled
 * 320 px RGB565 row and streams it. All the per-pixel work (scale mapping,
 * RGB555->RGB565, the volume HUD, scanline dimming) happens in
 * snes_band_push() on Core 0, inside the emulator's own frame. */
static void __not_in_flash_func(nes_blitter_core1)(void) {
    while (1) {
        if (!s_blitter_active) {
            tight_loop_contents();
            continue;
        }

        uint8_t r = s_read_slot;
        if (__atomic_load_n(&s_slot_state[r], __ATOMIC_ACQUIRE)) {
#ifndef ENABLE_DIAGNOSTIC_FRAME_METRICS
#define ENABLE_DIAGNOSTIC_FRAME_METRICS 0
#endif

#if ENABLE_DIAGNOSTIC_FRAME_METRICS
            {
                const uint8_t *px = s_slot_data[r];
                uint32_t nz = 0;
                for (uint32_t i = 0; i < NES_ROW_BYTES; i += 2)
                    if (px[i] || px[i + 1]) nz++;
                s_c1_rows++;
                s_c1_nz += nz;
                if (s_slot_row[r] != s_c1_last_row) {
                    s_c1_row_changes++;
                    s_c1_last_row = s_slot_row[r];
                }
            }
#endif
            ili9341_stream_row(s_slot_data[r], s_slot_row[r], s_slot_x0[r], s_slot_w[r]);
            __atomic_store_n(&s_slot_state[r], 0u, __ATOMIC_RELEASE);
            s_read_slot = (uint8_t)((r + 1u) & NES_LCD_SLOTS_MASK);
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
    slot_set_full_width(slot);

    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
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
        slot_set_full_width(slot);
        __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
        s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
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
    slot_set_full_width(slot);
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
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

static int s_md_screen_w = 256;
static int s_md_screen_h = 240;
static int s_md_panel_y = 0;
static uint16_t s_md_xmap[ILI9341_WIDTH];
static bool s_md_xmap_ready = false;

/* The DMG bezel window is not centred on the panel.
 *
 * Because the ILI9341 sits at an offset inside the Game Boy Classic shell cut,
 * the visible area is x = 45..310 (266 px) with asymmetric black borders of 45
 * px on the left and 9 px on the right. GB_MODE=1 in /boot.txt selects it.
 *
 * s_last_gb_mode exists so the lookup tables are rebuilt when that changes. It
 * is not paranoia: dmg_is_gb_mode() can flip between two reads of the same
 * /boot.txt, and a stale table would silently keep drawing the old geometry
 * with no error anywhere. This is the same class of bug the scale mode already
 * had to guard against. */
static int s_last_gb_mode = -1;

/* GB window state for the scanline path.
 *
 * Sampled once per frame from ili9341_stream_snes_frame_end(), never per
 * scanline. dmg_is_gb_mode() reaches /boot.txt the first time it is called, and
 * the blitter calls this function 224 times per frame; even with the shared
 * module's answer cached, putting a filesystem query in the inner loop is the
 * shape of bug that turns into a black screen the day the cache changes. */
static bool s_gb_mode_active = false;

void ili9341_sample_gb_mode(void) {
    s_gb_mode_active = dmg_is_gb_mode();
    s_last_gb_mode = s_gb_mode_active ? 1 : 0;
}

static int s_snes_source_h = 224;
static int s_md_dst_x = 0;
static int s_md_dst_y = 0;
static int s_md_dst_w = 256;
static int s_md_dst_h = 240;
static uint8_t s_snes_row_a[240];
static uint8_t s_snes_row_b[240];
static uint8_t s_stretch_row_for_line[2][240];
static bool s_stretch_tables_init = false;

static void md_prepare_band_xmap(void) {
    const int gb_mode = dmg_is_gb_mode() ? 1 : 0;
    s_gb_mode_active = (gb_mode != 0);

    if (s_md_xmap_ready && s_last_gb_mode == gb_mode) return;
    s_last_gb_mode = gb_mode;

    int src_w = s_md_screen_w;
    if (src_w <= 0) src_w = 256;
    int src_h = s_snes_source_h;
    if (src_h <= 0) src_h = 224;
    if (src_h > 240) src_h = 240;

    const int vx = gb_mode ? (int)DMG_GB_WINDOW_X : 0;
    const int vw = gb_mode ? (int)DMG_GB_WINDOW_W : ILI9341_WIDTH;
    const int vy = 0;
    const int vh = ILI9341_HEIGHT;

    int dx, dy, dw, dh;

    if (s_scale_mode == SCALE_INTEGER) {
        dw = src_w;
        dh = src_h;
        dx = vx + (vw - dw) / 2;
        dy = vy + (vh - dh) / 2;
    } else if (s_scale_mode == SCALE_STRETCH) {
        dw = vw;
        dh = vh;
        dx = vx;
        dy = vy;
    } else { /* SCALE_FIT */
        /* Proportional, preserving original aspect ratio src_w : src_h */
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

    /* The SPI column window follows the image rectangle. When it moves or
     * shrinks, columns that were image are now border and still hold pixels on
     * the panel: repaint everything full width for one frame per field. */
    if (dx != s_md_dst_x || dw != s_md_dst_w) {
        s_full_width_frames = 2u;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }

    s_md_dst_x = dx;
    s_md_dst_y = dy;
    s_md_dst_w = dw;
    s_md_dst_h = dh;
    s_md_panel_y = dy;

    /* Unified s_md_xmap covering all columns [0..ILI9341_WIDTH - 1] */
    for (uint16_t x = 0; x < ILI9341_WIDTH; ++x) {
        if (x < (uint16_t)dx || x >= (uint16_t)(dx + dw)) {
            s_md_xmap[x] = 0xFFFFu;
        } else {
            uint32_t lx = (uint32_t)((int)x - dx);
            s_md_xmap[x] = (uint16_t)((lx * (uint32_t)src_w) / (uint32_t)dw);
        }
    }

    /* Pre-compute panel row bounds for each source line:
     * s_snes_row_a[s] = first panel row
     * s_snes_row_b[s] = last panel row */
    for (int s = 0; s < 240; s++) {
        if (s < src_h && dh >= src_h) {
            int r_first = dy + (s * dh) / src_h;
            int r_last  = dy + ((s + 1) * dh - 1) / src_h;
            if (r_first >= ILI9341_HEIGHT) r_first = ILI9341_HEIGHT - 1;
            if (r_last >= ILI9341_HEIGHT)  r_last = ILI9341_HEIGHT - 1;
            s_snes_row_a[s] = (uint8_t)r_first;
            s_snes_row_b[s] = (uint8_t)r_last;
        } else {
            s_snes_row_a[s] = 0xFF;
            s_snes_row_b[s] = 0xFF;
        }
    }

    /* Stretch / interleave table mapping source line -> panel row */
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

void ili9341_snes_set_source_height(int source_h) {
    if (source_h <= 0) source_h = 224;
    if (source_h > 240) source_h = 240;
    if (s_snes_source_h != source_h) {
        s_snes_source_h = source_h;
        s_md_xmap_ready = false;
        md_prepare_band_xmap();
    }
}

void ili9341_snes_get_ymap(int source_line, uint8_t *first_row, uint8_t *last_row) {
    if (source_line >= 0 && source_line < 240) {
        if (first_row) *first_row = s_snes_row_a[source_line];
        if (last_row)  *last_row  = s_snes_row_b[source_line];
    }
}

static void md_band_push(const uint16_t *rgb565_line, uint8_t row);
static void snes_band_push(const uint16_t *rgb555_line, uint8_t row);

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
        slot_set_full_width(slot);
        __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
        s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
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
    slot_set_full_width(slot);
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
}

bool __not_in_flash_func(ili9341_is_active_md_line)(int line, int height) {
    if (!s_blitter_active || line < 0 || line >= height) return false;
    return true;
}

void __not_in_flash_func(ili9341_stream_md_line)(int line, const uint16_t *rgb565_line, int width, int height) {
    if (!s_blitter_active || line < 0 || line >= height) return;

    if (s_md_screen_w != width || s_snes_source_h != height) {
        s_md_screen_w = width;
        if (height > 0 && height <= 240) s_snes_source_h = height;
        s_md_xmap_ready = false;
        md_prepare_band_xmap();
    }

    if (s_scale_mode == SCALE_STRETCH && height == 224) {
        uint32_t r0 = ((uint32_t)line * 240u) / 224u;
        uint32_t r1 = (((uint32_t)line + 1u) * 240u) / 224u;
        if (r0 < ILI9341_HEIGHT) {
            md_band_push(rgb565_line, (uint8_t)r0);
        }
        if (r1 > (r0 + 1u) && (r0 + 1u) < ILI9341_HEIGHT) {
            md_band_push(rgb565_line, (uint8_t)(r0 + 1u));
        }
    } else {
        uint8_t row = (uint8_t)(s_md_panel_y + line);
        if (row < ILI9341_HEIGHT) {
            md_band_push(rgb565_line, row);
        }
    }
}

void __not_in_flash_func(ili9341_stream_md_frame_end)(void) {
    if (!s_blitter_active) return;

    /* Toggle field parity every frame for interlaced 60 fps */
    s_field_parity ^= 1u;
}

static void __not_in_flash_func(snes_band_push)(const uint16_t *rgb555_line, uint8_t row) {
    if (!s_blitter_active || row >= ILI9341_HEIGHT) return;

    if (rgb555_line == NULL) {
        if (s_row_hash[row] == 0u) return;
        s_row_hash[row] = 0u;

        const uint8_t slot = s_write_slot;
        while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
            tight_loop_contents();
        }
        memset(s_slot_data[slot], 0, NES_ROW_BYTES);
        s_slot_row[slot] = row;
        slot_set_full_width(slot);
        __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
        s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
        return;
    }

    const bool dim = s_scanlines_enabled && (row & 1u);
    const uint32_t *src32 = (const uint32_t *)rgb555_line;
    uint32_t h = 2166136261u;
    int chunks = s_md_screen_w >> 1;
    int i = 0;
    for (; i <= chunks - 4; i += 4) {
        h = (h ^ src32[i]) * 16777619u;
        h = (h ^ src32[i+1]) * 16777619u;
        h = (h ^ src32[i+2]) * 16777619u;
        h = (h ^ src32[i+3]) * 16777619u;
    }
    for (; i < chunks; i++) {
        h = (h ^ src32[i]) * 16777619u;
    }

    if (dim) {
        h ^= 0x5A5A5A5Au;
    }

    /* Ask for the shared volume HUD row only for rows 180..199 where HUD lives */
    int hud_x = 0, hud_w = 0;
    const uint16_t *hud = (row >= 180 && row < 200) ? dmg_volume_hud_get_row(row, &hud_x, &hud_w) : NULL;
    if (hud != NULL && hud_w > 0) {
        h ^= 0x48554441u; /* 'HUDA' -- forces a rebuild while the HUD is up */
        const int chunks_h = hud_w >> 1;
        for (int i = 0; i < chunks_h; i++) {
            h = (h ^ ((const uint32_t *)hud)[i]) * 16777619u;
        }
    }

    /* Low-battery overlay: resend the row when it appears, changes or hides */
    dmg_batt_overlay_mark_row(row);
    s_bov_frame_rows = true;
    const bool bov_row = (row >= dmg_batt_overlay_y0() && row < dmg_batt_overlay_y0() + dmg_batt_overlay_h());
    h ^= dmg_batt_overlay_row_salt(row);

    if (h == s_row_hash[row] && (!s_fps_enabled || row < 8 || row >= 16)) return;
    s_row_hash[row] = h;

    if (!s_md_xmap_ready) md_prepare_band_xmap();
    const uint16_t *xmap = s_md_xmap;

    const uint8_t slot = s_write_slot;
    if (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        /* Queue full: Core 1 is behind on SPI. Count it -- this is the number
         * that says whether the panel bus, not the emulator, set the frame time. */
        const uint32_t t_wait = time_us_32();
        while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
            tight_loop_contents();
        }
        s_core0_wait_us += time_us_32() - t_wait;
    }

    uint16_t *dst16 = (uint16_t *)s_slot_data[slot];
    const int max_w = s_md_screen_w;
    const int convert_w = (max_w <= 256) ? max_w : 256;

    uint16_t line565[256];
    if (!dim) {
        int i = 0;
        for (; i <= convert_w - 4; i += 4) {
            line565[i+0] = __builtin_bswap16(rgb555_to_rgb565(rgb555_line[i+0]));
            line565[i+1] = __builtin_bswap16(rgb555_to_rgb565(rgb555_line[i+1]));
            line565[i+2] = __builtin_bswap16(rgb555_to_rgb565(rgb555_line[i+2]));
            line565[i+3] = __builtin_bswap16(rgb555_to_rgb565(rgb555_line[i+3]));
        }
        for (; i < convert_w; i++) {
            line565[i] = __builtin_bswap16(rgb555_to_rgb565(rgb555_line[i]));
        }
    } else {
        int i = 0;
        for (; i <= convert_w - 4; i += 4) {
            uint16_t c0 = rgb555_to_rgb565(rgb555_line[i+0]);
            uint16_t c1 = rgb555_to_rgb565(rgb555_line[i+1]);
            uint16_t c2 = rgb555_to_rgb565(rgb555_line[i+2]);
            uint16_t c3 = rgb555_to_rgb565(rgb555_line[i+3]);
            if (c0 != 0) c0 = (c0 >> 1) & 0x7BEF;
            if (c1 != 0) c1 = (c1 >> 1) & 0x7BEF;
            if (c2 != 0) c2 = (c2 >> 1) & 0x7BEF;
            if (c3 != 0) c3 = (c3 >> 1) & 0x7BEF;
            line565[i+0] = __builtin_bswap16(c0);
            line565[i+1] = __builtin_bswap16(c1);
            line565[i+2] = __builtin_bswap16(c2);
            line565[i+3] = __builtin_bswap16(c3);
        }
        for (; i < convert_w; i++) {
            uint16_t c = rgb555_to_rgb565(rgb555_line[i]);
            if (c != 0) c = (c >> 1) & 0x7BEF;
            line565[i] = __builtin_bswap16(c);
        }
    }

    /* Column window for this row. Rows carrying an overlay that may sit outside
     * the image (FPS digits at x = 8, the volume HUD) and every row during the
     * full-width repaint after a geometry change go out at full width, so their
     * border columns are rewritten black; everything else is caged to the image. */
    uint16_t win_x0 = (uint16_t)s_md_dst_x;
    uint16_t win_w = (uint16_t)s_md_dst_w;
    if (s_full_width_frames != 0u || (hud != NULL && hud_w > 0) || bov_row ||
        (s_fps_enabled && row >= 8 && row < 16) ||
        s_md_dst_x < 0 || s_md_dst_w <= 0 || s_md_dst_x + s_md_dst_w > (int)ILI9341_WIDTH) {
        win_x0 = 0u;
        win_w = ILI9341_WIDTH;
    }
    s_rows_pushed_this_frame = true;

    /* Compose only the window (rounded out to the 4-pixel unroll; the extra
     * columns are inside the 320-px slot and are not sent). */
    const uint16_t comp_x0 = (uint16_t)(win_x0 & ~3u);
    uint16_t comp_x1 = (uint16_t)((win_x0 + win_w + 3u) & ~3u);
    if (comp_x1 > ILI9341_WIDTH) comp_x1 = ILI9341_WIDTH;

    for (uint16_t x = comp_x0; x < comp_x1; x += 4) {
        uint16_t m0 = xmap[x+0];
        uint16_t m1 = xmap[x+1];
        uint16_t m2 = xmap[x+2];
        uint16_t m3 = xmap[x+3];
        dst16[x+0] = (m0 >= max_w) ? 0x0000 : line565[m0];
        dst16[x+1] = (m1 >= max_w) ? 0x0000 : line565[m1];
        dst16[x+2] = (m2 >= max_w) ? 0x0000 : line565[m2];
        dst16[x+3] = (m3 >= max_w) ? 0x0000 : line565[m3];
    }

    /* Composite the shared volume OSD over the game pixels.
     *
     * The HUD is pre-composed in RAM by dmg_volume_hud_render() on the Core 0 /
     * menu path, so this only has to blend it in. 0x0001 is the shared module's
     * transparent sentinel: leave the game pixel underneath it.
     *
     * Doing it here, in the band composition, is what keeps the framerate
     * untouched. The alternative -- drawing on the display from Core 0 --
     * contends with Core 1 for the SPI bus, and when an emulator in this project
     * did it that way it cost roughly 15 FPS along with visible glitches. */
    if (hud != NULL && hud_w > 0) {
        for (int i = 0; i < hud_w; i++) {
            const int px = hud_x + i;
            if (px < 0 || px >= (int)ILI9341_WIDTH) continue;
            const uint16_t c = hud[i];
            if (c == 0x0001u) continue; /* transparent: keep the game pixel */
            dst16[px] = __builtin_bswap16(c);
        }
    }

    if (bov_row) dmg_batt_overlay_stamp_row(row, dst16, 0, ILI9341_WIDTH, true);

    /* Direct FPS overlay on lines 8..15 */
    if (s_fps_enabled && row >= 8 && row < 16) {
        int r = (int)row - 8;
        /* In GB_MODE=1, x = 8 lands in the 45 px black bezel border, which is
         * under the plastic of the Game Boy shell and therefore invisible. The
         * overlay has to start inside the window instead. */
        int x_pos = s_gb_mode_active ? ((int)DMG_GB_WINDOW_X + 8) : 8;
        for (int i = 0; s_fps_text[i] != '\0' && (x_pos + 8) < ILI9341_WIDTH; i++) {
            char slice = get_font_slice(s_fps_text[i], r);
            for (int bit = 0; bit < 8; bit++) {
                uint16_t c = (slice & (1 << bit)) ? 0x07E0 : 0x0000;
                dst16[x_pos + bit] = __builtin_bswap16(c);
            }
            x_pos += 8;
        }
    }

    s_slot_row[slot] = row;
    s_slot_x0[slot] = win_x0;
    s_slot_w[slot] = win_w;
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
}

/* Whole-frame pixel accounting.
 *
 * The first diagnostic sampled s_screen once a second, which is one scanline of
 * scratch, reused per line -- so a "0 non-zero pixels" reading only meant the line
 * that happened to be sampled was black. It could not tell "the PPU renders
 * nothing" apart from "this game has a black scanline", which are opposite
 * faults in opposite places.
 *
 * This counts every pixel of every scanline the core actually emits, so the
 * number answers the question directly: out of the full frame, how much was drawn.
 *
 * Large number + black screen  -> the fault is downstream, in this driver.
 * Near zero                     -> the fault is in the core's rasteriser.
 */
static volatile uint32_t s_frame_nonzero = 0;
static volatile uint32_t s_frame_lines = 0;
static volatile uint32_t s_frame_checksum = 0;

uint32_t ili9341_frame_nonzero(void) { return s_frame_nonzero; }
uint32_t ili9341_frame_lines(void) { return s_frame_lines; }
uint32_t ili9341_frame_checksum(void) { return s_frame_checksum; }
void ili9341_frame_reset(void) {
    s_frame_nonzero = 0;
    s_frame_lines = 0;
    s_frame_checksum = 0;
}

void ili9341_c1_stats(uint32_t *rows, uint32_t *nonzero, uint32_t *row_changes) {
    if (rows)        *rows        = s_c1_rows;
    if (nonzero)     *nonzero     = s_c1_nz;
    if (row_changes) *row_changes = s_c1_row_changes;
}

void ili9341_c1_stats_reset(void) {
    /* Read-and-clear from Core 0 while Core 1 keeps appending. Each word is 32-bit
     * aligned, so a single load/store is atomic on Cortex-M33 and losing an
     * increment to a concurrent clear is harmless -- the next window counts it.
     * Subtracting would not be, which is why this clears rather than accumulating. */
    s_c1_rows = 0;
    s_c1_nz = 0;
    s_c1_row_changes = 0;
}

void __not_in_flash_func(ili9341_stream_snes_line)(int line, const uint16_t *rgb555_line, int width, int height) {
    if (!s_blitter_active || line < 0 || line >= ILI9341_HEIGHT) return;

    /* Geometry is fixed for the whole session on this path (port_glue.cpp passes
     * the panel height, never the core's PPU.ScreenHeight), so there is nothing to
     * recompute here. Re-deriving it on every call is what made the picture jump
     * between the 224 and 239 layouts mid-frame when a game wrote the SNES
     * overscan register $2133. */
    if (s_md_screen_w != width || s_snes_source_h != height) {
        s_md_screen_w = width;
        if (height > 0 && height <= 240) s_snes_source_h = height;
        s_md_xmap_ready = false;
        md_prepare_band_xmap();
    }

#if ENABLE_DIAGNOSTIC_FRAME_METRICS
    /* Count the emitted pixels BEFORE any scaling or filtering, so this measures
     * what the core produced and not what this driver did with it. That
     * separation is the whole point: it decides whether a black screen belongs to
     * the emulator or to the display path. */
    {
        uint32_t nz = 0, sum = 0;
        for (int x = 0; x < width; x++) {
            const uint16_t c = rgb555_line[x];
            if (c != 0) nz++;
            sum = (sum * 33u) ^ c;
        }
        s_frame_nonzero += nz;
        s_frame_lines++;
        s_frame_checksum = sum;
    }
#endif

    if (s_scale_mode == SCALE_INTEGER) {
        /* 1:1 Pixel Native: centered vertically at s_md_dst_y */
        int row = s_md_dst_y + line;
        if (row >= 0 && row < ILI9341_HEIGHT) {
            if (!s_md_interleave || ((row & 1u) == s_field_parity)) {
                snes_band_push(rgb555_line, (uint8_t)row);
            }
        }
    } else {
        /* Stretch and Fit: scaled vertically via s_stretch_row_for_line */
        if (!s_stretch_tables_init) md_prepare_band_xmap();
        if (s_md_interleave) {
            uint8_t row = s_stretch_row_for_line[s_field_parity][line];
            if (row != 0xFF && row < ILI9341_HEIGHT) {
                snes_band_push(rgb555_line, row);
            }
        } else {
            for (int p = 0; p < 2; p++) {
                uint8_t row = s_stretch_row_for_line[p][line];
                if (row != 0xFF && row < ILI9341_HEIGHT) {
                    snes_band_push(rgb555_line, row);
                }
            }
        }
    }
}


/* Low-battery overlay rows the game did not emit this frame (above/below the
 * image): queued as black full-width rows with the overlay stamped. Hash 0 =
 * "black row", so the NULL-line path above does not repaint them blank. */
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
        while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) tight_loop_contents();
        memset(s_slot_data[slot], 0, NES_ROW_BYTES);
        dmg_batt_overlay_stamp_row(row, (uint16_t *)s_slot_data[slot], 0, ILI9341_WIDTH, true);
        s_row_hash[row] = 0u;
        s_slot_row[slot] = (uint8_t)row;
        slot_set_full_width(slot);
        __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
        s_write_slot = (uint8_t)((slot + 1u) & NES_LCD_SLOTS_MASK);
    }
}

void __not_in_flash_func(ili9341_stream_snes_frame_end)(void) {
    /* End of frame: refresh the window sample so a change in /boot.txt is picked
     * up, and so the next frame's lookup tables can be rebuilt if it moved. */
    ili9341_sample_gb_mode();
    if (!s_blitter_active) return;
    bov_queue_rows();
    /* Count down the full-width repaint only on frames that actually pushed
     * rows; a frameskipped frame repaints nothing. */
    if (s_rows_pushed_this_frame && s_full_width_frames != 0u) {
        s_full_width_frames--;
    }
    s_rows_pushed_this_frame = false;
    if (s_md_interleave) {
        s_field_parity ^= 1u;
    }
}

void ili9341_blitter_start(void) {
    if (s_blitter_active) return;

    /* Launch Core 1 blitter once on the first invocation */
    if (!s_blitter_core1_launched) {
        s_blitter_core1_launched = true;
        multicore_launch_core1(nes_blitter_core1);
    }

    ili9341_sample_gb_mode();
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
    s_full_width_frames = 2u;
    s_rows_pushed_this_frame = false;

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

bool ili9341_is_blitter_active(void) {
    return s_blitter_active;
}

void ili9341_set_interleave(bool enable) {
    if (s_md_interleave != enable) {
        s_md_interleave = enable;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_interleave(void) {
    return s_md_interleave;
}

uint8_t ili9341_get_field_parity(void) {
    return s_field_parity;
}

bool ili9341_snes_line_active(int line) {
    if (!s_md_interleave) return true;
    if (line < 0 || line >= 240) return false;
    if (s_scale_mode == SCALE_INTEGER) {
        return (((s_md_dst_y + line) & 1u) == s_field_parity);
    } else {
        if (!s_stretch_tables_init) md_prepare_band_xmap();
        return (s_stretch_row_for_line[s_field_parity][line] != 0xFF);
    }
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
        /* When not in stretch mode, explicitly clear top and bottom borders if blitter is active */
        if (s_blitter_active && s_scale_mode != SCALE_STRETCH) {
            int top_border = s_md_dst_y;
            int bot_border = s_md_dst_y + s_md_dst_h;
            for (int r = 0; r < top_border && r < ILI9341_HEIGHT; r++) {
                snes_band_push(NULL, (uint8_t)r);
            }
            for (int r = bot_border; r < ILI9341_HEIGHT; r++) {
                snes_band_push(NULL, (uint8_t)r);
            }
        }
    }}

ili9341_scale_mode_t ili9341_get_scale_mode(void) {
    return s_scale_mode;
}

void ili9341_draw_char(char c, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg) {
    if (x + 8 > ILI9341_WIDTH || y + 8 > ILI9341_HEIGHT) return;
    if (s_fb) {
        for (int r = 0; r < 8; r++) {
            char slice = get_font_slice(c, r);
            uint16_t *line = &s_fb[(y + r) * ILI9341_WIDTH + x];
            for (int col = 0; col < 8; col++) {
                line[col] = (slice & (1 << col)) ? fg : bg;
            }
        }
        return;
    }

    /* Direct SPI render when no framebuffer is allocated */
    ili9341_wait_frame_done();
    set_addr_window(x, y, (uint16_t)(x + 7), (uint16_t)(y + 7));
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    uint16_t fg565 = rgb555_to_rgb565(fg);
    uint16_t bg565 = rgb555_to_rgb565(bg);
    uint16_t char_pixels[64];
    for (int r = 0; r < 8; r++) {
        char slice = get_font_slice(c, r);
        for (int col = 0; col < 8; col++) {
            char_pixels[r * 8 + col] = (slice & (1 << col)) ? fg565 : bg565;
        }
    }
    spi_write16_blocking(ILI9341_SPI, char_pixels, 64);

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
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
    if (!s_fb) return;
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
    if (!s_fb) return;
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

    if (s_fb) {
        for (int r = y; r < y + h; r++) {
            uint16_t *line = &s_fb[r * ILI9341_WIDTH + x];
            for (int c = 0; c < w; c++) {
                line[c] = color;
            }
        }
        return;
    }

    /* Direct SPI render when no framebuffer is allocated */
    ili9341_wait_frame_done();
    set_addr_window((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    uint16_t col565 = rgb555_to_rgb565(color);
    static uint16_t line_buf[ILI9341_WIDTH];
    for (int i = 0; i < w; i++) line_buf[i] = col565;

    for (int r = 0; r < h; r++) {
        spi_write16_blocking(ILI9341_SPI, line_buf, (size_t)w);
    }

    while (spi_is_busy(ILI9341_SPI)) tight_loop_contents();
    while (spi_is_readable(ILI9341_SPI)) (void)spi_get_hw(ILI9341_SPI)->dr;

    cs_high();
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void ili9341_fill_screen(uint16_t color) {
    if (s_fb) {
        for (int i = 0; i < ILI9341_WIDTH * ILI9341_HEIGHT; i++) {
            s_fb[i] = color;
        }
        ili9341_flush(true);
    } else {
        ili9341_fill_rect(0, 0, ILI9341_WIDTH, ILI9341_HEIGHT, color);
    }
}
