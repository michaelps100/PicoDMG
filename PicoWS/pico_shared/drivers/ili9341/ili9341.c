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
#define NES_LCD_SLOTS 8u
#define NES_ROW_BYTES (ILI9341_WIDTH * 2u)

static uint8_t __aligned(4) s_slot_data[NES_LCD_SLOTS][NES_ROW_BYTES];
static uint8_t s_slot_row[NES_LCD_SLOTS];
/* Horizontal span of a slot that Core 1 has to send, in panel pixels. Pixel data
 * stays indexed by absolute x inside the slot, so overlays (FPS, volume HUD) do not
 * care. w == 0 means "full 320-pixel row": producers that do not know about spans
 * leave it alone, and Core 1 clears it again after every row. */
static uint16_t s_slot_x0[NES_LCD_SLOTS];
static uint16_t s_slot_w[NES_LCD_SLOTS];
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
static inline bool bov_in_rows(uint16_t row) {
    return row >= dmg_batt_overlay_y0() && row < dmg_batt_overlay_y0() + dmg_batt_overlay_h();
}
/* Per-second WonderSwan profiling counters (see ili9341_ws_prof_take). Written
 * from Core 0 except c1_us, which Core 1 owns; plain 32-bit stores, no locking. */
static volatile ili9341_ws_prof_t s_prof;
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
 * serves either panel: ST7789= in /boot.txt decides at runtime.
 *
 * The measured values are in AGENTS.md of the repo root, section 7.1: the
 * ST7789 needs 0x60 in landscape (0x28 plus MX) where the ILI9341 needs 0x28, and
 * the two differ in colour order as well. Nothing here is derived -- the
 * mirror bits depend on how the controller is bonded to the glass, so they are
 * measured, not computed. */
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
 * having been debugged separately.
 *
 * Note what is NOT here: PWCTR1..5, VMCTR1/2, DFUNCTR, INVCTR, GAMSET and the
 * two gamma registers. Those are ILI9341-only; the ST7789 has no equivalent and
 * sending them to a ST7789 either does nothing or leaves the panel in whatever
 * state the last command put it in. */
static void st7789_init_sequence(void) {
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    write_cmd(dmg_panel_get_invert() ? CMD_INVON : CMD_INVOFF);

    write_cmd(CMD_COLMOD);
    write_data(0x55); /* 16-bit RGB565 */

    apply_madctl();

/* Physical panel timing. The factory ST7789 default sweeps GRAM at ~60 Hz
     * (16.6 ms), which is the same order as the time the Core 1 blitter takes to
     * push a frame over SPI, so the write and the scan collide in GRAM and show
     * up as a horizontal tear. Overclocking the sweep to ~111 Hz (~9 ms)
     * decouples the two.
     * Values live in shared_menu/dmg_panel.c -- one place, so a panel timing fix
     * is not applied to some emulators and forgotten in others. */
    dmg_panel_st7789_timing(panel_timing_write);

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    write_cmd(CMD_NORON);
    sleep_ms(10);

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
     * the sequences below, which read the resolved values.
     *
     * NOTE: this needs the SD card already mounted, because dmg_panel reads
     * /boot.txt. It tries several paths (0:/boot.txt, /boot.txt, boot.txt) and
     * tolerates the card being absent, falling back to DMG_DEFAULT_PANEL_ST7789,
     * so calling it before f_mount is survivable but would always pick the
     * default. PicoWS's main() mounts the SD first for that reason. */
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

    nes_prepare_band_xmap();
    memset(s_fb, 0, sizeof(s_fb));
    ili9341_fill_screen(0x0000);
    ili9341_set_backlight(true);

    printf("PicoWS display: %s MADCTL=0x%02X (%s) %s (%s) @ %u Hz (Core 1 streaming)\n",
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
static char s_fps_text[40] = "60";

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

/* ------------------------------------------------------------------ */
/* Core 1 row streaming                                                */
/*                                                                     */
/* A row used to cost three CS-framed transactions (PASET, RAMWR, data), */
/* each waiting for the SPI engine to go idle: ~9 us of dead time on top */
/* of the 73 us the 640 data bytes need at 70 MHz. With every row of a   */
/* progressive Fit frame (205) that alone pushed the frame over 16.7 ms. */
/* The window is now opened once and rows that follow each other simply  */
/* keep feeding the same RAMWR, with CS held low. A new window is only   */
/* issued when the row is not the next one, or the column span changes.  */
/* ------------------------------------------------------------------ */
static bool     s_win_open = false;
static uint16_t s_win_x0 = 0u;
static uint16_t s_win_w = 0u;
static uint16_t s_win_next_row = 0u;

/* Waits for the shifter to finish and drops anything that came back. */
static inline void __not_in_flash_func(spi_settle)(void) {
    spi_hw_t *hw = spi_get_hw(ILI9341_SPI);
    while (hw->sr & SPI_SSPSR_BSY_BITS) tight_loop_contents();
    while (hw->sr & SPI_SSPSR_RNE_BITS) (void)hw->dr;
}

/* Byte feed that lives in RAM: spi_write_blocking() sits in flash and shares
 * the XIP cache with the ROM and the video renderer running on Core 0. */
static void __not_in_flash_func(spi_feed)(const uint8_t *p, size_t n) {
    spi_hw_t *hw = spi_get_hw(ILI9341_SPI);
    while (n) {
        const uint32_t sr = hw->sr;
        if (sr & SPI_SSPSR_RNE_BITS) (void)hw->dr;
        if (sr & SPI_SSPSR_TNF_BITS) {
            hw->dr = *p++;
            n--;
        }
    }
}

static void __not_in_flash_func(spi_command)(uint8_t cmd, const uint8_t *args, size_t n) {
    spi_settle();
    dc_cmd();
    spi_feed(&cmd, 1);
    if (n) {
        spi_settle();
        dc_data();
        spi_feed(args, n);
    }
}

/* Ends a streamed window: lets the last bytes out and releases CS. */
static void __not_in_flash_func(ili9341_close_window)(void) {
    if (!s_win_open) return;
    spi_settle();
    cs_high();
    s_win_open = false;
}

/* Sends one slot to the panel, opening a new window only when needed. */
static void __not_in_flash_func(ili9341_stream_slot)(uint8_t slot) {
    const uint16_t row = s_slot_row[slot];
    uint16_t x0 = s_slot_x0[slot];
    uint16_t w  = s_slot_w[slot];
    s_slot_x0[slot] = 0u;
    s_slot_w[slot]  = 0u;
    if (w == 0u) {
        x0 = 0u;
        w = ILI9341_WIDTH;
    }
    if (row >= ILI9341_HEIGHT || (uint32_t)x0 + w > ILI9341_WIDTH) return;

    if (!s_win_open || x0 != s_win_x0 || w != s_win_w || row != s_win_next_row) {
        if (s_win_open) {
            spi_settle();
            cs_high();
        }
        cs_low();
        const uint16_t x1 = (uint16_t)(x0 + w - 1u);
        const uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                                   (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
        const uint8_t paset[4] = { (uint8_t)(row >> 8), (uint8_t)(row & 0xFF),
                                   (uint8_t)((ILI9341_HEIGHT - 1) >> 8),
                                   (uint8_t)((ILI9341_HEIGHT - 1) & 0xFF) };
        spi_command(CMD_CASET, caset, 4);
        spi_command(CMD_PASET, paset, 4);
        spi_command(CMD_RAMWR, NULL, 0);
        spi_settle();
        dc_data();
        s_win_open = true;
        s_win_x0 = x0;
        s_win_w = w;
    }

    spi_feed(s_slot_data[slot] + 2u * x0, 2u * (size_t)w);
    s_win_next_row = (uint16_t)(row + 1u);
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
            const uint32_t t0 = timer_hw->timerawl;
            ili9341_stream_slot(r);
            s_prof.c1_us += timer_hw->timerawl - t0;
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
 * WonderSwan Streaming Support (224x144 8-bit indexed â†’ 320x240 RGB565)
 * ========================================================================= */
#define WS_SRC_W  224
#define WS_SRC_H  144

static uint16_t s_ws_xmap[ILI9341_WIDTH];
static bool     s_ws_xmap_ready = false;
static bool     s_shimmerless_enabled = true;
static dmg_scale_weight_t s_ws_xweights[ILI9341_WIDTH];

/* Vertical stretch tables: 144 source lines â†’ 240 display rows */
static uint8_t  s_ws_stretch_rows[2][WS_SRC_H][2];
static uint8_t  s_ws_stretch_row_cnt[2][WS_SRC_H];
static uint8_t  s_ws_prog_rows[WS_SRC_H][2];
static uint8_t  s_ws_prog_row_cnt[WS_SRC_H];
static bool     s_ws_stretch_inited = false;
static volatile bool s_ws_interleave = true;
static volatile bool s_ws_vertical = false;
/* Active span of the horizontal image on the panel, set by ws_prepare_xmap() */
static uint16_t s_ws_ax0 = 0u;
static uint16_t s_ws_aw  = ILI9341_WIDTH;

/* palette LUT: index 0..15 â†’ RGB565 big-endian (bswapped for SPI) */
static uint16_t s_ws_pal[16];
static bool     s_ws_pal_valid = false;
/* Same 16 mono shades in native byte order, for the blend path (one bswap per
 * output pixel instead of three). */
static uint16_t s_ws_pal_native[16];

/* Bumped whenever a WonderSwan Color palette *bank* (16 entries) really changes
 * colour. The row hashes cover only tile indices, so without this a game that
 * recolours a palette without touching a tile would be invisible: every row would
 * hash the same and be skipped, leaving stale colours and gaps on screen.
 *
 * It used to be one global counter bumped on every palette byte written, even with
 * an unchanged value. A game that re-uploads its palette every frame therefore
 * invalidated all 144 rows every frame and the dirty-row skip never worked in WSC.
 * Per-bank versions fold into a row's hash only for the banks that row actually
 * uses, and only move on a visible change. */
static uint32_t s_ws_bank_ver[16];

void ili9341_ws_prof_take(ili9341_ws_prof_t *out) {
    memcpy(out, (const void *)&s_prof, sizeof(*out));
    memset((void *)&s_prof, 0, sizeof(s_prof));
}

void ili9341_ws_palette_changed(uint32_t bank) {
    s_ws_bank_ver[bank & 15u]++;
    s_prof.epoch_bumps++;
}
static uint32_t s_ws_pal_hash = 0;

extern uint8_t  ws_gpu_colorActive; /* colour path active (system is WSC and the game set port 0x60 bit 7) */
extern uint16_t wsc_palette_rgb565[256];        /* byte-swapped for SPI */
extern uint16_t wsc_palette_rgb565_native[256]; /* native order, for blending */

static void ws_update_palette_lut(const uint32_t *ws_shades) {
    if (!ws_shades) return;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; i++) {
        h = (h ^ ws_shades[i]) * 16777619u;
    }
    if (h == s_ws_pal_hash && s_ws_pal_valid) return;
    s_ws_pal_hash = h;
    for (int i = 0; i < 16; i++) {
        uint32_t rgb = ws_shades[i]; /* (R<<16)|(G<<8)|B */
        uint32_t r = (rgb >> 16) & 0xFF;
        uint32_t g = (rgb >>  8) & 0xFF;
        uint32_t b =  rgb        & 0xFF;
        uint16_t r5 = (uint16_t)(r >> 3);
        uint16_t g6 = (uint16_t)(g >> 2);
        uint16_t b5 = (uint16_t)(b >> 3);
        uint16_t c = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        s_ws_pal[i] = __builtin_bswap16(c);
        s_ws_pal_native[i] = c;
    }
    s_ws_pal_valid = true;
    for (int i = 0; i < ILI9341_HEIGHT; i++) {
        s_row_hash[i] = 0xFFFFFFFFu;
    }
}

void ili9341_invalidate_ws_palette(void) {
    s_ws_pal_valid = false;
    s_ws_pal_hash = 0;
    for (int i = 0; i < ILI9341_HEIGHT; i++) {
        s_row_hash[i] = 0xFFFFFFFFu;
    }
}

static void ws_prepare_xmap(void) {
    if (s_ws_xmap_ready) return;
    int disp_x = 0;
    int disp_w = ILI9341_WIDTH;
    if (dmg_is_gb_mode()) {
        if (s_scale_mode == SCALE_INTEGER) {
            disp_x = 66;
            disp_w = WS_SRC_W;
        } else {
            disp_x = 45;
            disp_w = 266;
        }
    } else {
        if (s_scale_mode == SCALE_INTEGER) {
            disp_x = 48;
            disp_w = WS_SRC_W;
        } else {
            disp_x = 0;
            disp_w = ILI9341_WIDTH;
        }
    }

    /* The span Core 1 has to send: everything outside it is black margin that is
     * already on the panel, so it is not worth a single SPI byte per row. */
    s_ws_ax0 = (uint16_t)disp_x;
    s_ws_aw  = (uint16_t)disp_w;

    if (s_scale_mode == SCALE_INTEGER) {
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            if (x < disp_x || x >= disp_x + disp_w) {
                s_ws_xmap[x] = 0xFFFFu;
                s_ws_xweights[x].src_idx = 0xFFFFu;
                s_ws_xweights[x].weight1 = 0;
            } else {
                int src = x - disp_x;
                s_ws_xmap[x] = (uint16_t)src;
                s_ws_xweights[x].src_idx = (uint16_t)src;
                s_ws_xweights[x].weight1 = 0;
            }
        }
    } else {
        dmg_scale_weight_t temp[ILI9341_WIDTH];
        dmg_init_shimmerless_weights((uint16_t)WS_SRC_W, (uint16_t)disp_w, temp);
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            if (x < disp_x || x >= disp_x + disp_w) {
                s_ws_xmap[x] = 0xFFFFu;
                s_ws_xweights[x].src_idx = 0xFFFFu;
                s_ws_xweights[x].weight1 = 0;
            } else {
                int lx = x - disp_x;
                s_ws_xmap[x] = (uint16_t)(((uint32_t)lx * WS_SRC_W) / (uint32_t)disp_w);
                s_ws_xweights[x] = temp[lx];
            }
        }
    }
    s_ws_xmap_ready = true;
}

static void init_ws_stretch_tables(void) {
    if (s_ws_stretch_inited) return;
    memset(s_ws_stretch_row_cnt, 0, sizeof(s_ws_stretch_row_cnt));
    memset(s_ws_prog_row_cnt,    0, sizeof(s_ws_prog_row_cnt));

    int y_start = 0, y_end = ILI9341_HEIGHT;
    if (dmg_is_gb_mode()) {
        if (s_scale_mode == SCALE_FIT) {
            /* 144*(266/224) â‰ˆ 171 rows, centered in 240 â†’ (240-171)/2 = 34 */
            int h = (int)(((uint32_t)WS_SRC_H * 266u) / WS_SRC_W);
            if (h > ILI9341_HEIGHT) h = ILI9341_HEIGHT;
            y_start = (ILI9341_HEIGHT - h) / 2;
            y_end   = y_start + h;
        }
    } else {
        if (s_scale_mode == SCALE_FIT) {
            /* 144*(320/224) â‰ˆ 205 rows, centered â†’ top border (240-205)/2 = 17 */
            int h = (int)(((uint32_t)WS_SRC_H * ILI9341_WIDTH) / WS_SRC_W);
            if (h > ILI9341_HEIGHT) h = ILI9341_HEIGHT;
            y_start = (ILI9341_HEIGHT - h) / 2;
            y_end   = y_start + h;
        }
    }

    for (int r = y_start; r < y_end; r++) {
        int s   = ((r - y_start) * WS_SRC_H) / (y_end - y_start);
        if (s >= WS_SRC_H) s = WS_SRC_H - 1;
        int par = r & 1;
        uint8_t cnt = s_ws_stretch_row_cnt[par][s];
        if (cnt < 2) {
            s_ws_stretch_rows[par][s][cnt] = (uint8_t)r;
            s_ws_stretch_row_cnt[par][s]   = cnt + 1;
        }
        uint8_t pcnt = s_ws_prog_row_cnt[s];
        if (pcnt < 2) {
            s_ws_prog_rows[s][pcnt] = (uint8_t)r;
            s_ws_prog_row_cnt[s]    = pcnt + 1;
        }
    }
    s_ws_stretch_inited = true;
}

/* Hash of one indexed source line, including the palette state it depends on.
 *
 * The palette is NOT covered by the pixel words alone, and it must be: a
 * WonderSwan Color game writes new RGB444 entries straight into palette RAM while
 * the tile indices stay identical. Same indices + different palette = different
 * pixels but an identical hash, so the row was skipped and stale colours stayed
 * on screen (this is what left gaps in Digimon Tamers' menus).
 *
 * Only the palette banks the line actually references (index >> 4) are folded in,
 * so a palette change elsewhere on screen does not invalidate this row. */
static uint32_t __not_in_flash_func(ws_line_hash)(const uint8_t *idx, int bytes) {
    const uint32_t *p = (const uint32_t *)idx;
    const bool color = ws_gpu_colorActive;
    uint32_t h = 2166136261u;
    uint32_t used = 0u;
    for (int i = 0; i < (bytes >> 2); i++) {
        const uint32_t w = p[i];
        h = (h ^ w) * 16777619u;
        if (color) {
            const uint32_t b = (w >> 4) & 0x0F0F0F0Fu;
            used |= (1u << (b & 0xFu)) | (1u << ((b >> 8) & 0xFu)) |
                    (1u << ((b >> 16) & 0xFu)) | (1u << (b >> 24));
        }
    }
    while (used) {
        const uint32_t bank = (uint32_t)__builtin_ctz(used);
        used &= used - 1u;
        h = (h ^ s_ws_bank_ver[bank] ^ (bank << 28)) * 16777619u;
    }
    return h;
}

/* ~14% darken for the Grid effect, RGB565 in native byte order */
static inline uint16_t ws_dim565(uint16_t c) {
    uint32_t r5 = (c >> 11) & 0x1Fu;
    uint32_t g6 = (c >>  5) & 0x3Fu;
    uint32_t b5 =  c        & 0x1Fu;
    r5 = (r5 * 220u) >> 8;
    g6 = (g6 * 220u) >> 8;
    b5 = (b5 * 220u) >> 8;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* Source line resolved to native RGB565 once, so the Shimmerless blend does not
 * look every neighbour up (and byte-swap it) again for each output pixel. With Grid
 * the darkened copy is built here too: 224 darkenings per source line instead of one
 * per output pixel, shared by both panel rows of a Fit line. Blending two darkened
 * neighbours equals darkening the blend (the Grid dim is a linear scale). */
static uint16_t s_ws_src565[WS_SRC_W + 1];
static uint16_t s_ws_src565_dim[WS_SRC_W + 1];

static void __not_in_flash_func(ws_resolve_src)(const uint8_t *indexed_224, bool apply_grid) {
    const bool do_blend = s_shimmerless_enabled && (s_scale_mode != SCALE_INTEGER);
    if (!do_blend && !apply_grid) return;   /* ws_emit_row reads the big-endian LUT directly */
    const uint32_t imask   = ws_gpu_colorActive ? 0xFFu : 0x0Fu;
    const uint16_t *lut_nat = ws_gpu_colorActive ? wsc_palette_rgb565_native : s_ws_pal_native;
    for (int i = 0; i < WS_SRC_W; i++) {
        s_ws_src565[i] = lut_nat[indexed_224[i] & imask];
    }
    s_ws_src565[WS_SRC_W] = s_ws_src565[WS_SRC_W - 1];
    if (apply_grid) {
        for (int i = 0; i <= WS_SRC_W; i++) {
            s_ws_src565_dim[i] = ws_dim565(s_ws_src565[i]);
        }
    }
}

/* Emits one panel row of the source line last passed to ws_resolve_src() as the RGB565
 * big-endian row of the slot, for the active span only (the margins are not sent).
 * Grid darkens odd rows entirely and odd columns of even rows. */
static void __not_in_flash_func(ws_emit_row)(uint16_t *dst16, const uint8_t *indexed_224,
                                             uint8_t row, bool apply_grid) {
    const uint32_t x0 = s_ws_ax0;
    const uint32_t x1 = (uint32_t)s_ws_ax0 + s_ws_aw;
    const bool do_blend = s_shimmerless_enabled && (s_scale_mode != SCALE_INTEGER);

    if (!do_blend && !apply_grid) {
        const uint32_t imask  = ws_gpu_colorActive ? 0xFFu : 0x0Fu;
        const uint16_t *lut_be = ws_gpu_colorActive ? wsc_palette_rgb565 : s_ws_pal;
        for (uint32_t x = x0; x < x1; x++) {
            dst16[x] = lut_be[indexed_224[s_ws_xmap[x]] & imask];
        }
        return;
    }

    const bool row_dim = apply_grid && (row & 1u);
    const uint16_t *src[2];
    src[0] = row_dim ? s_ws_src565_dim : s_ws_src565;      /* even columns */
    src[1] = apply_grid ? s_ws_src565_dim : s_ws_src565;   /* odd columns */

    if (!do_blend) {
        for (uint32_t x = x0; x < x1; x++) {
            dst16[x] = __builtin_bswap16(src[x & 1u][s_ws_xmap[x]]);
        }
        return;
    }

    for (uint32_t x = x0; x < x1; x++) {
        const uint16_t *sp = src[x & 1u];
        const uint32_t i0 = s_ws_xweights[x].src_idx;
        const uint8_t  w1 = s_ws_xweights[x].weight1;
        uint16_t c = sp[i0];
        if (w1) c = dmg_blend_rgb565(c, sp[i0 + 1u], w1);
        dst16[x] = __builtin_bswap16(c);
    }
}

/* FPS counter and volume HUD stamped on a finished row (absolute x coordinates) */
static void __not_in_flash_func(ws_slot_overlay)(uint16_t *dst16, uint8_t row) {
    if (s_fps_enabled && row >= 8 && row < 16) {
        int r = (int)row - 8;
        int xp = dmg_is_gb_mode() ? (45 + 8) : 8;
        int max_x = dmg_is_gb_mode() ? 311 : ILI9341_WIDTH;
        uint8_t *dst8 = (uint8_t *)dst16;
        for (int i = 0; s_fps_text[i] != '\0' && (xp + 8) < max_x; i++) {
            char sl = get_font_slice(s_fps_text[i], r);
            for (int bit = 0; bit < 8; bit++) {
                uint16_t c = (sl & (1 << bit)) ? 0x07E0 : 0x0000;
                dst8[2*(xp+bit)+0] = (uint8_t)(c >> 8);
                dst8[2*(xp+bit)+1] = (uint8_t)(c & 0xFF);
            }
            xp += 8;
        }
    }

    /* Volume OSD HUD overlay */
    int hud_x, hud_w;
    const uint16_t *hud = dmg_volume_hud_get_row(row, &hud_x, &hud_w);
    if (hud) {
        uint8_t *dst8 = (uint8_t *)dst16;
        for (int i = 0; i < hud_w; i++) {
            uint16_t c = hud[i];
            if (c != 0x0001) {
                dst8[2 * (hud_x + i) + 0] = (uint8_t)(c >> 8);
                dst8[2 * (hud_x + i) + 1] = (uint8_t)(c & 0xFF);
            }
        }
    }
}

/* Waits until Core 1 has released the slot, accounting the time lost. */
static inline void __not_in_flash_func(ws_wait_slot)(uint8_t slot) {
    if (!__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) return;
    const uint32_t t0 = timer_hw->timerawl;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }
    s_prof.spin_us += timer_hw->timerawl - t0;
}

bool __not_in_flash_func(ili9341_is_active_ws_line)(int line) {
    if (!s_blitter_active || line < 0 || line >= WS_SRC_H) return false;
    if (!s_ws_interleave) return true;
    if (s_scale_mode == SCALE_INTEGER) {
        uint8_t row = (uint8_t)(48 + line);
        return (row < ILI9341_HEIGHT) && ((row & 1u) == s_field_parity);
    }
    init_ws_stretch_tables();
    return (s_ws_stretch_row_cnt[s_field_parity][line] > 0);
}

void __not_in_flash_func(ili9341_stream_ws_line)(int line, const uint8_t *indexed_224, const uint32_t *ws_shades_rgb888) {
    if (!s_blitter_active || line < 0 || line >= WS_SRC_H || !indexed_224) return;

    const uint32_t t_start = timer_hw->timerawl;

    /* The 16 mono shades are irrelevant in colour mode (it has its own 256-entry LUT) */
    if (!ws_gpu_colorActive) ws_update_palette_lut(ws_shades_rgb888);
    ws_prepare_xmap();

    const bool grid = s_grid_effect;

    /* Panel rows this source line lands on: 1 (Integer, or Fit rows that map 1:1)
     * or 2 (Fit stretches 144 source lines over 205 rows). */
    uint8_t rows[2];
    uint8_t cnt = 0;
    if (s_scale_mode == SCALE_INTEGER) {
        const uint8_t row = (uint8_t)(48 + line);
        if (row < ILI9341_HEIGHT && (!s_ws_interleave || (row & 1u) == s_field_parity)) {
            rows[cnt++] = row;
        }
    } else {
        init_ws_stretch_tables();
        const uint8_t *map;
        uint8_t n;
        if (s_ws_interleave) {
            n = s_ws_stretch_row_cnt[s_field_parity][line];
            map = s_ws_stretch_rows[s_field_parity][line];
        } else {
            n = s_ws_prog_row_cnt[line];
            map = s_ws_prog_rows[line];
        }
        for (uint8_t i = 0; i < n; i++) {
            if (map[i] < ILI9341_HEIGHT) rows[cnt++] = map[i];
        }
    }
    if (cnt == 0) return;

    /* Hashed once per source line, not once per panel row */
    const uint32_t h_line = ws_line_hash(indexed_224, WS_SRC_W);

    bool need[2] = { false, false };
    uint32_t rh[2];
    uint8_t nneed = 0;
    /* The dirty-row hash covers the game image only, so a row whose game pixels did not
     * change was skipped -- and the volume HUD, which is stamped onto the row after the
     * hash, never showed on a still scene. While the HUD is up its rows are always sent
     * and their hash is poisoned, so the first frame after it expires repaints them
     * clean instead of leaving the old HUD on the panel. */
    const bool hud_on = dmg_volume_hud_is_active();
    for (uint8_t i = 0; i < cnt; i++) {
        const uint8_t row = rows[i];
        rh[i] = h_line;
        if (grid && (row & 1u)) rh[i] ^= 0x5A5A5A5Au;
        dmg_batt_overlay_mark_row(row);
        s_bov_frame_rows = true;
        rh[i] ^= dmg_batt_overlay_row_salt(row);
        if (hud_on && row >= 180 && row < 200) {
            s_row_hash[row] = 0xFFFFFFFFu;
            need[i] = true;
            nneed++;
            continue;
        }
        if (rh[i] == s_row_hash[row] && (!s_fps_enabled || row < 8 || row >= 16)) {
            s_prof.rows_skipped++;
            continue;
        }
        s_row_hash[row] = rh[i];
        need[i] = true;
        nneed++;
    }
    if (nneed == 0) {
        s_prof.push_us += timer_hw->timerawl - t_start;
        return;
    }

    /* Convert the line once; a second panel row of the same source line (without
     * Grid, where the two rows differ) is a plain copy of the first. Overlays are
     * stamped afterwards, on each row, so the copy stays clean. */
    const uint8_t first = s_write_slot;
    const size_t span_bytes = 2u * (size_t)s_ws_aw;
    uint16_t *pristine = NULL;
    bool resolved = false;
    uint8_t slots[2];
    uint8_t srows[2];
    uint8_t np = 0;
    for (uint8_t i = 0; i < cnt; i++) {
        if (!need[i]) continue;
        const uint8_t slot = (uint8_t)((first + np) % NES_LCD_SLOTS);
        ws_wait_slot(slot);
        uint16_t *dst16 = (uint16_t *)s_slot_data[slot];
        if (pristine != NULL) {
            memcpy(&dst16[s_ws_ax0], &pristine[s_ws_ax0], span_bytes);
        } else {
            if (!resolved) {
                ws_resolve_src(indexed_224, grid);
                resolved = true;
            }
            ws_emit_row(dst16, indexed_224, rows[i], grid);
            if (!grid) pristine = dst16;
        }
        slots[np] = slot;
        srows[np] = rows[i];
        np++;
    }

    for (uint8_t j = 0; j < np; j++) {
        uint16_t *dst16 = (uint16_t *)s_slot_data[slots[j]];
        ws_slot_overlay(dst16, srows[j]);
        s_slot_row[slots[j]] = srows[j];
        s_slot_x0[slots[j]]  = s_ws_ax0;
        s_slot_w[slots[j]]   = s_ws_aw;
        if (bov_in_rows(srows[j])) {
            /* low-battery overlay rows go out full width: the overlay reaches past
             * the image span, and the next send after it hides wipes the border */
            const uint16_t ax1 = (uint16_t)(s_ws_ax0 + s_ws_aw);
            if (s_ws_ax0 > 0u) memset(dst16, 0, 2u * (size_t)s_ws_ax0);
            if (ax1 < ILI9341_WIDTH) memset(&dst16[ax1], 0, 2u * (size_t)(ILI9341_WIDTH - ax1));
            dmg_batt_overlay_stamp_row(srows[j], dst16, 0, ILI9341_WIDTH, true);
            s_slot_x0[slots[j]] = 0u;
            s_slot_w[slots[j]]  = ILI9341_WIDTH;
        }
        __atomic_store_n(&s_slot_state[slots[j]], 1u, __ATOMIC_RELEASE);
    }
    s_write_slot = (uint8_t)((first + np) % NES_LCD_SLOTS);
    s_prof.rows_pushed += np;
    s_prof.push_us += timer_hw->timerawl - t_start;
}

static bool s_ws_fps_drawn = false;

void __not_in_flash_func(ili9341_stream_ws_frame_end)(void) {
    if (!s_blitter_active) return;

    if (!s_ws_vertical && (s_scale_mode == SCALE_INTEGER || s_scale_mode == SCALE_FIT)) {
        if (s_fps_enabled) {
            s_ws_fps_drawn = true;
            for (uint8_t row = 8; row < 16; row++) {
                if (!s_ws_interleave || (row & 1u) == s_field_parity) {
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
                    dmg_batt_overlay_stamp_row(row, (uint16_t *)dst, 0, ILI9341_WIDTH, true);
                    s_slot_row[slot] = row;
                    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
                    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
                }
            }
        } else if (s_ws_fps_drawn) {
            for (uint8_t row = 8; row < 16; row++) {
                const uint8_t slot = s_write_slot;
                while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
                    tight_loop_contents();
                }
                memset(s_slot_data[slot], 0, ILI9341_WIDTH * 2);
                dmg_batt_overlay_stamp_row(row, (uint16_t *)s_slot_data[slot], 0, ILI9341_WIDTH, true);
                s_slot_row[slot] = row;
                __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
                s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
                s_row_hash[row] = 0u;
            }
            s_ws_fps_drawn = false;
        }
    }

    /* Low-battery overlay rows the image does not cover: black + overlay */
    if (s_bov_frame_rows) {
        s_bov_frame_rows = false;
        const uint32_t m = dmg_batt_overlay_frame_rows();
        const uint16_t y0 = dmg_batt_overlay_y0();
        for (uint16_t i = 0; i < dmg_batt_overlay_h(); i++) {
            if (!(m & (1u << i))) continue;
            const uint16_t row = (uint16_t)(y0 + i);
            const uint8_t slot = s_write_slot;
            while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) tight_loop_contents();
            memset(s_slot_data[slot], 0, ILI9341_WIDTH * 2);
            dmg_batt_overlay_stamp_row(row, (uint16_t *)s_slot_data[slot], 0, ILI9341_WIDTH, true);
            s_slot_row[slot] = (uint8_t)row;
            s_slot_x0[slot] = 0u;
            s_slot_w[slot] = ILI9341_WIDTH;
            __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
            s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
            s_row_hash[row] = 0u;
        }
    }

    if (s_ws_interleave) s_field_parity ^= 1u;
}

/* =========================================================================
 * WonderSwan Vertical (Tate) Streaming Support (144x224 â†’ 320x240 RGB565)
 * ========================================================================= */
#define WS_VERT_W 144
#define WS_VERT_H 224

static uint16_t s_ws_vxmap[ILI9341_WIDTH];
static bool     s_ws_vxmap_ready = false;
/* Image span of a Tate row: only these pixels are sent, the side borders are cleared once
 * when the mode is entered (clear_black on toggle, game start and menu exit). */
static uint16_t s_ws_vx0 = 0u;
static uint16_t s_ws_vw  = ILI9341_WIDTH;
/* Rows last sent full width because an overlay (FPS, volume HUD) reached into the borders;
 * their next send is full width too, which wipes the overlay off the borders. */
static bool     s_ws_vrow_wide[ILI9341_HEIGHT];
static dmg_scale_weight_t s_ws_vxweights[ILI9341_WIDTH];

static uint8_t  s_ws_vstretch_rows[2][WS_VERT_H][2];
static uint8_t  s_ws_vstretch_row_cnt[2][WS_VERT_H];
static uint8_t  s_ws_vprog_rows[WS_VERT_H][2];
static uint8_t  s_ws_vprog_row_cnt[WS_VERT_H];
static bool     s_ws_vstretch_inited = false;

void ili9341_set_ws_vertical(bool vertical) {
    if (s_ws_vertical != vertical) {
        s_ws_vertical = vertical;
        s_ws_vxmap_ready = false;
        s_ws_vstretch_inited = false;
        for (int i = 0; i < ILI9341_HEIGHT; i++) {
            s_row_hash[i] = 0xFFFFFFFFu;
        }
    }
}

bool ili9341_get_ws_vertical(void) {
    return s_ws_vertical;
}

static void ws_prepare_vxmap(void) {
    if (s_ws_vxmap_ready) return;
    int disp_x = 0;
    int disp_w = ILI9341_WIDTH;
    if (dmg_is_gb_mode()) {
        if (s_scale_mode == SCALE_INTEGER) {
            disp_x = 106;
            disp_w = WS_VERT_W;
        } else if (s_scale_mode == SCALE_FIT) {
            disp_x = 101;
            disp_w = 154;
        } else {
            disp_x = 45;
            disp_w = 266;
        }
    } else {
        if (s_scale_mode == SCALE_INTEGER) {
            disp_x = 88;
            disp_w = WS_VERT_W;
        } else if (s_scale_mode == SCALE_FIT) {
            disp_x = 83;
            disp_w = 154;
        } else {
            disp_x = 0;
            disp_w = ILI9341_WIDTH;
        }
    }

    if (s_scale_mode == SCALE_INTEGER) {
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            if (x < disp_x || x >= disp_x + disp_w) {
                s_ws_vxmap[x] = 0xFFFFu;
                s_ws_vxweights[x].src_idx = 0xFFFFu;
                s_ws_vxweights[x].weight1 = 0;
            } else {
                int src = x - disp_x;
                s_ws_vxmap[x] = (uint16_t)src;
                s_ws_vxweights[x].src_idx = (uint16_t)src;
                s_ws_vxweights[x].weight1 = 0;
            }
        }
    } else {
        dmg_scale_weight_t temp[ILI9341_WIDTH];
        dmg_init_shimmerless_weights((uint16_t)WS_VERT_W, (uint16_t)disp_w, temp);
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            if (x < disp_x || x >= disp_x + disp_w) {
                s_ws_vxmap[x] = 0xFFFFu;
                s_ws_vxweights[x].src_idx = 0xFFFFu;
                s_ws_vxweights[x].weight1 = 0;
            } else {
                int lx = x - disp_x;
                s_ws_vxmap[x] = (uint16_t)(((uint32_t)lx * WS_VERT_W) / (uint32_t)disp_w);
                s_ws_vxweights[x] = temp[lx];
            }
        }
    }
    s_ws_vx0 = (uint16_t)disp_x;
    s_ws_vw  = (uint16_t)disp_w;
    s_ws_vxmap_ready = true;
}

static void init_ws_vstretch_tables(void) {
    if (s_ws_vstretch_inited) return;
    memset(s_ws_vstretch_row_cnt, 0, sizeof(s_ws_vstretch_row_cnt));
    memset(s_ws_vprog_row_cnt,    0, sizeof(s_ws_vprog_row_cnt));

    int y_start = 0, y_end = ILI9341_HEIGHT;
    if (s_scale_mode == SCALE_INTEGER) {
        /* Centered vertically: (240 - 224) / 2 = 8 px borders */
        y_start = 8;
        y_end   = 232;
    }

    for (int r = y_start; r < y_end; r++) {
        int s   = ((r - y_start) * WS_VERT_H) / (y_end - y_start);
        if (s >= WS_VERT_H) s = WS_VERT_H - 1;
        int par = r & 1;
        uint8_t cnt = s_ws_vstretch_row_cnt[par][s];
        if (cnt < 2) {
            s_ws_vstretch_rows[par][s][cnt] = (uint8_t)r;
            s_ws_vstretch_row_cnt[par][s]   = cnt + 1;
        }
        uint8_t pcnt = s_ws_vprog_row_cnt[s];
        if (pcnt < 2) {
            s_ws_vprog_rows[s][pcnt] = (uint8_t)r;
            s_ws_vprog_row_cnt[s]    = pcnt + 1;
        }
    }
    s_ws_vstretch_inited = true;
}

static void __not_in_flash_func(ws_vband_push)(const uint8_t *indexed_144, uint8_t row, bool apply_grid) {
    if (!s_blitter_active || row >= ILI9341_HEIGHT) return;

    uint32_t h = ws_line_hash(indexed_144, WS_VERT_W);
    if (apply_grid && (row & 1u)) h ^= 0x5A5A5A5Au;
    dmg_batt_overlay_mark_row(row);
    s_bov_frame_rows = true;
    h ^= dmg_batt_overlay_row_salt(row);

    /* volume HUD rows are always sent while it shows (see ili9341_stream_ws_line) */
    if (row >= 180 && row < 200 && dmg_volume_hud_is_active()) {
        s_row_hash[row] = 0xFFFFFFFFu;
    } else {
        if (h == s_row_hash[row] && (!s_fps_enabled || row < 8 || row >= 16)) return;
        s_row_hash[row] = h;
    }

    ws_prepare_vxmap();

    const bool overlay = (s_fps_enabled && row >= 8 && row < 16) ||
                         (row >= 180 && row < 200 && dmg_volume_hud_is_active()) ||
                         dmg_batt_overlay_row_hit(row);
    const bool wide = overlay || s_ws_vrow_wide[row];
    s_ws_vrow_wide[row] = overlay;
    const uint16_t xlo = wide ? 0u : s_ws_vx0;
    const uint16_t xhi = wide ? (uint16_t)ILI9341_WIDTH : (uint16_t)(s_ws_vx0 + s_ws_vw);

    const uint8_t slot = s_write_slot;
    while (__atomic_load_n(&s_slot_state[slot], __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    uint16_t *dst16 = (uint16_t *)s_slot_data[slot];
    const bool row_dim = apply_grid && (row & 1u);
    const bool do_blend = s_shimmerless_enabled && (s_scale_mode != SCALE_INTEGER);

    if (!apply_grid) {
        if (!do_blend) {
            if (ws_gpu_colorActive) {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t mx = s_ws_vxmap[x];
                    if (mx == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t idx = indexed_144[mx];
                    dst16[x] = wsc_palette_rgb565[idx];
                }
            } else {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t mx = s_ws_vxmap[x];
                    if (mx == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t idx = indexed_144[mx] & 0x0Fu;
                    dst16[x] = s_ws_pal[idx];
                }
            }
        } else {
            if (ws_gpu_colorActive) {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t i0 = s_ws_vxweights[x].src_idx;
                    if (i0 == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t w1 = s_ws_vxweights[x].weight1;
                    uint16_t c0 = __builtin_bswap16(wsc_palette_rgb565[indexed_144[i0]]);
                    if (w1 == 0 || (i0 + 1u) >= WS_VERT_W) {
                        dst16[x] = __builtin_bswap16(c0);
                    } else {
                        uint16_t c1 = __builtin_bswap16(wsc_palette_rgb565[indexed_144[i0 + 1u]]);
                        dst16[x] = __builtin_bswap16(dmg_blend_rgb565(c0, c1, w1));
                    }
                }
            } else {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t i0 = s_ws_vxweights[x].src_idx;
                    if (i0 == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t w1 = s_ws_vxweights[x].weight1;
                    uint16_t c0 = __builtin_bswap16(s_ws_pal[indexed_144[i0] & 0x0Fu]);
                    if (w1 == 0 || (i0 + 1u) >= WS_VERT_W) {
                        dst16[x] = __builtin_bswap16(c0);
                    } else {
                        uint16_t c1 = __builtin_bswap16(s_ws_pal[indexed_144[i0 + 1u] & 0x0Fu]);
                        dst16[x] = __builtin_bswap16(dmg_blend_rgb565(c0, c1, w1));
                    }
                }
            }
        }
    } else {
        if (!do_blend) {
            if (ws_gpu_colorActive) {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t mx = s_ws_vxmap[x];
                    if (mx == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t idx = indexed_144[mx];
                    uint16_t c  = __builtin_bswap16(wsc_palette_rgb565[idx]);
                    if (row_dim || (x & 1u)) {
                        /* ~14% darken */
                        uint32_t r5 = (c >> 11) & 0x1Fu;
                        uint32_t g6 = (c >>  5) & 0x3Fu;
                        uint32_t b5 =  c        & 0x1Fu;
                        r5 = (r5 * 220u) >> 8;
                        g6 = (g6 * 220u) >> 8;
                        b5 = (b5 * 220u) >> 8;
                        c  = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            } else {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t mx = s_ws_vxmap[x];
                    if (mx == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t idx = indexed_144[mx] & 0x0Fu;
                    uint16_t c  = __builtin_bswap16(s_ws_pal[idx]);
                    if (row_dim || (x & 1u)) {
                        /* ~14% darken */
                        uint32_t r5 = (c >> 11) & 0x1Fu;
                        uint32_t g6 = (c >>  5) & 0x3Fu;
                        uint32_t b5 =  c        & 0x1Fu;
                        r5 = (r5 * 220u) >> 8;
                        g6 = (g6 * 220u) >> 8;
                        b5 = (b5 * 220u) >> 8;
                        c  = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            }
        } else {
            if (ws_gpu_colorActive) {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t i0 = s_ws_vxweights[x].src_idx;
                    if (i0 == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t w1 = s_ws_vxweights[x].weight1;
                    uint16_t c0 = __builtin_bswap16(wsc_palette_rgb565[indexed_144[i0]]);
                    uint16_t c;
                    if (w1 == 0 || (i0 + 1u) >= WS_VERT_W) {
                        c = c0;
                    } else {
                        uint16_t c1 = __builtin_bswap16(wsc_palette_rgb565[indexed_144[i0 + 1u]]);
                        c = dmg_blend_rgb565(c0, c1, w1);
                    }
                    if (row_dim || (x & 1u)) {
                        uint32_t r5 = (c >> 11) & 0x1Fu;
                        uint32_t g6 = (c >>  5) & 0x3Fu;
                        uint32_t b5 =  c        & 0x1Fu;
                        r5 = (r5 * 220u) >> 8;
                        g6 = (g6 * 220u) >> 8;
                        b5 = (b5 * 220u) >> 8;
                        c  = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            } else {
                for (uint16_t x = xlo; x < xhi; x++) {
                    uint16_t i0 = s_ws_vxweights[x].src_idx;
                    if (i0 == 0xFFFFu) { dst16[x] = 0; continue; }
                    uint8_t w1 = s_ws_vxweights[x].weight1;
                    uint16_t c0 = __builtin_bswap16(s_ws_pal[indexed_144[i0] & 0x0Fu]);
                    uint16_t c;
                    if (w1 == 0 || (i0 + 1u) >= WS_VERT_W) {
                        c = c0;
                    } else {
                        uint16_t c1 = __builtin_bswap16(s_ws_pal[indexed_144[i0 + 1u] & 0x0Fu]);
                        c = dmg_blend_rgb565(c0, c1, w1);
                    }
                    if (row_dim || (x & 1u)) {
                        uint32_t r5 = (c >> 11) & 0x1Fu;
                        uint32_t g6 = (c >>  5) & 0x3Fu;
                        uint32_t b5 =  c        & 0x1Fu;
                        r5 = (r5 * 220u) >> 8;
                        g6 = (g6 * 220u) >> 8;
                        b5 = (b5 * 220u) >> 8;
                        c  = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                    }
                    dst16[x] = __builtin_bswap16(c);
                }
            }
        }
    }

    /* FPS overlay */
    if (s_fps_enabled && row >= 8 && row < 16) {
        int r = (int)row - 8;
        int xp = dmg_is_gb_mode() ? (45 + 8) : 8;
        int max_x = dmg_is_gb_mode() ? 311 : ILI9341_WIDTH;
        uint8_t *dst8 = (uint8_t *)dst16;
        for (int i = 0; s_fps_text[i] != '\0' && (xp + 8) < max_x; i++) {
            char sl = get_font_slice(s_fps_text[i], r);
            for (int bit = 0; bit < 8; bit++) {
                uint16_t c = (sl & (1 << bit)) ? 0x07E0 : 0x0000;
                dst8[2*(xp+bit)+0] = (uint8_t)(c >> 8);
                dst8[2*(xp+bit)+1] = (uint8_t)(c & 0xFF);
            }
            xp += 8;
        }
    }

    /* Volume OSD HUD overlay */
    int hud_x, hud_w;
    const uint16_t *hud = dmg_volume_hud_get_row(row, &hud_x, &hud_w);
    if (hud) {
        uint8_t *dst8 = (uint8_t *)dst16;
        for (int i = 0; i < hud_w; i++) {
            uint16_t c = hud[i];
            if (c != 0x0001) {
                dst8[2 * (hud_x + i) + 0] = (uint8_t)(c >> 8);
                dst8[2 * (hud_x + i) + 1] = (uint8_t)(c & 0xFF);
            }
        }
    }

    dmg_batt_overlay_stamp_row(row, dst16, 0, ILI9341_WIDTH, true);

    s_slot_row[slot] = row;
    s_slot_x0[slot]  = xlo;
    s_slot_w[slot]   = (uint16_t)(xhi - xlo);
    __atomic_store_n(&s_slot_state[slot], 1u, __ATOMIC_RELEASE);
    s_write_slot = (uint8_t)((slot + 1u) % NES_LCD_SLOTS);
}

void __not_in_flash_func(ili9341_stream_ws_vline)(int vline, const uint8_t *indexed_144, const uint32_t *ws_shades_rgb888) {
    if (!s_blitter_active || vline < 0 || vline >= WS_VERT_H || !indexed_144) return;

    ws_update_palette_lut(ws_shades_rgb888);

    const bool grid = s_grid_effect;

    if (s_scale_mode == SCALE_INTEGER) {
        uint8_t row = (uint8_t)(8 + vline);
        if (row >= ILI9341_HEIGHT) return;
        if (s_ws_interleave && ((row & 1u) != s_field_parity)) return;
        ws_vband_push(indexed_144, row, grid);
    } else {
        init_ws_vstretch_tables();
        if (s_ws_interleave) {
            uint8_t cnt = s_ws_vstretch_row_cnt[s_field_parity][vline];
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t row = s_ws_vstretch_rows[s_field_parity][vline][i];
                if (row < ILI9341_HEIGHT) ws_vband_push(indexed_144, row, grid);
            }
        } else {
            uint8_t cnt = s_ws_vprog_row_cnt[vline];
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t row = s_ws_vprog_rows[vline][i];
                if (row < ILI9341_HEIGHT) ws_vband_push(indexed_144, row, grid);
            }
        }
    }
}


void ili9341_set_interleave(bool enable) {
    /* The WonderSwan path has its own flag (s_ws_interleave). It used to start at
     * true and nothing ever wrote it, so "Screen: Progressive" in the menu changed
     * only the NGP variable and the console was interlaced no matter what was chosen. */
    if (s_ngp_interleave != enable || s_ws_interleave != enable) {
        s_ngp_interleave = enable;
        s_ws_interleave = enable;
        s_field_parity = 0u;
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

    s_win_open = false;
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
    s_win_open = false;

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
        s_ws_xmap_ready = false;
        s_ws_stretch_inited = false;
        ws_prepare_xmap();
        s_ws_vxmap_ready = false;
        s_ws_vstretch_inited = false;
        ws_prepare_vxmap();
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
    /* Every caller is about to drive the SPI bus itself. Slots are released as
     * soon as their bytes are in the FIFO, and Core 1 keeps the streamed window
     * (and CS) open between rows, so close it here. Core 1 is idle now. */
    ili9341_close_window();
}

bool ili9341_is_frame_busy(void) {
    for (int i = 0; i < NES_LCD_SLOTS; i++) {
        if (__atomic_load_n(&s_slot_state[i], __ATOMIC_ACQUIRE)) return true;
    }
    return false;
}

/* Menu synchronous flush (flushes full 320x240 s_fb with RGB555->RGB565 conversion) */
/* One line buffer for the three blocking flushes below. They all run on core 0
 * and never overlap, and three separate statics were SRAM PicoWS did not have. */
static uint16_t s_flush_line[ILI9341_WIDTH];

void ili9341_flush(bool wait) {
    (void)wait;
    ili9341_wait_frame_done();

    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    write_cmd(CMD_RAMWR);

    dc_data();
    cs_low();
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    uint16_t *line_buf = s_flush_line;
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

    uint16_t *line_buf = s_flush_line;
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

    uint16_t *line_buf = s_flush_line;
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
