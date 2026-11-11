/*
 * ili9341.h - Native pico-sdk SPI driver for 320x240 TFT displays.
 *
 * Pin mapping follows the PicoDMG convention:
 *   CS  = GPIO17
 *   SCK = GPIO18
 *   MOSI= GPIO19
 *   DC  = GPIO20
 *   RST = GPIO21
 *   BL  = GPIO22
 *
 * Two panels are supported behind this one driver, selected at runtime with
 * ili9341_set_panel() before ili9341_init(): the original ILI9341 and the
 * ST7789. They share the MIPI DCS framing, the CASET/PASET/RAMWR addressing,
 * the 16-bit DMA transfer and the GB_MODE letterbox, so only the power-on
 * sequence and MADCTL differ -- see panel_init_sequence() in ili9341.c. The
 * ST7789 sequence is the one already proven by PicoGBC's mk_st7789.c.
 *
 * The driver maintains an internal 320x240 RGB565 framebuffer in SRAM.
 * Callers render into the framebuffer; ili9341_flush() sends the whole frame
 * to the display via hardware SPI + DMA.
 */

#ifndef ILI9341_H
#define ILI9341_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ILI9341_SPI_INST
#define ILI9341_SPI     ILI9341_SPI_INST
#else
#ifndef ILI9341_SPI
#define ILI9341_SPI     spi0
#endif
#endif

#ifndef ILI9341_PIN_CS
#define ILI9341_PIN_CS  17
#endif

#ifndef ILI9341_PIN_SCK
#define ILI9341_PIN_SCK 18
#endif

#ifndef ILI9341_PIN_MOSI
#define ILI9341_PIN_MOSI 19
#endif

#ifndef ILI9341_PIN_DC
#define ILI9341_PIN_DC  20
#endif

#ifndef ILI9341_PIN_RST
#define ILI9341_PIN_RST 21
#endif

#ifndef ILI9341_PIN_BL
#define ILI9341_PIN_BL  22
#endif

#define ILI9341_WIDTH   320
#define ILI9341_HEIGHT  240

void ili9341_init(void);
void ili9341_set_backlight(bool on);
void ili9341_set_rotation(uint8_t rotation);
uint8_t ili9341_apply_default_rotation(uint8_t rotation);

/* Raw MADCTL (0x36) escape hatch, for panels whose mirror bits do not match
 * what set_rotation() derives. Applied verbatim -- the caller owns the bit
 * layout (MY 0x80, MX 0x40, MV 0x20, ML 0x10, BGR 0x08, MH 0x04). It also
 * counts as a rotation choice, so apply_default_rotation() will not re-derive
 * over it, and set_rotation() will not overwrite it once set. Backed by
 * boot.txt's MADCTL= key. */
void    ili9341_set_madctl(uint8_t madctl);
uint8_t ili9341_get_madctl(void);

/* Which controller is on the SPI bus. Must be set before ili9341_init() to
 * take effect; changing it afterwards only affects the accessors. Defaults to
 * DMG_PANEL_ILI9341 so every existing caller keeps the current behaviour. */
typedef enum {
    DMG_PANEL_ILI9341 = 0,
    DMG_PANEL_ST7789  = 1,
} dmg_panel_t;

void        ili9341_set_panel(dmg_panel_t panel);
dmg_panel_t ili9341_get_panel(void);

/* Colour inversion (CMD_INVON / CMD_INVOFF). INVERT=1 in /boot.txt maps to
 * on. Default off, which is also what the ILI9341 sequence used to do by
 * simply never emitting the command. */
void ili9341_set_invert(bool on);
bool ili9341_get_invert(void);

/* GRAM window offset in pixels, for a panel whose usable area does not start
 * at (0,0). 0/0 is correct for the module this project uses; the keys exist so
 * a different panel can be accommodated without a rebuild. */
void ili9341_set_gram_offset(uint8_t xoff, uint8_t yoff);
void ili9341_get_gram_offset(uint8_t *xoff, uint8_t *yoff);

uint16_t *ili9341_get_framebuffer(void);
void ili9341_flush(bool wait);
void ili9341_flush_rows(int y_start, int y_end);
void ili9341_flush_async(void);
bool ili9341_flush_busy(void);

void ili9341_write_pixel(int x, int y, uint16_t color);
void ili9341_fill_rect(int x, int y, int w, int h, uint16_t color);
void ili9341_fill_screen(uint16_t color);

void ili9341_set_gb_mode(bool enabled);

/* Framebuffer pixel format for the next flushes: false = RGB444 0x0RGB (the
 * bootloader's native UI format, default), true = RGB565 sent unchanged.
 * The graphical menu turns it on only around its own frame flush. */
void ili9341_set_native_565(bool enabled);
bool ili9341_get_native_565(void);

/* Horizontal hardware scroll over screen columns x..x+w-1 (landscape only).
 * begin() returns false when the current MADCTL/GRAM offset can't scroll that
 * way -- callers fall back to full-frame redraws. set(p): screen column x+k
 * shows what was written at column x+((k+p) mod w). end() restores normal
 * display mode; call it once the window holds the final image again. */
bool ili9341_hscroll_begin(int x, int w);
void ili9341_hscroll_set(int p);
void ili9341_hscroll_end(void);

/* Write screen columns x0..x1 of all 240 rows, RGB565, bypassing the
 * framebuffer. row() fills dst[0..n-1] with row y, columns x0..x0+n-1; it is
 * called for row y+1 while row y is still going out over DMA. */
typedef void (*ili9341_row_fn)(int y, uint16_t *dst, int x0, int n, void *ctx);
void ili9341_write_cols_565(int x0, int x1, ili9341_row_fn row, void *ctx);

/* Send the rectangle [x0..x1] x [y0..y1] of fb (320-wide, format per
 * ili9341_set_native_565) one screen column at a time. In landscape the panel
 * scans along X, so moving content written this way tears at most along one
 * vertical seam instead of shearing diagonally. Meant for small areas. */
void ili9341_write_rect_by_columns(const uint16_t *fb, int x0, int x1, int y0, int y1);
bool ili9341_get_gb_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* ILI9341_H */
