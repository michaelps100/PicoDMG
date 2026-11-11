#ifndef ILI9341_H
#define ILI9341_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ILI9341_SPI
#define ILI9341_SPI      spi0
#endif

#ifndef ILI9341_PIN_CS
#define ILI9341_PIN_CS   17
#endif
#ifndef ILI9341_PIN_SCK
#define ILI9341_PIN_SCK  18
#endif
#ifndef ILI9341_PIN_MOSI
#define ILI9341_PIN_MOSI 19
#endif
#ifndef ILI9341_PIN_DC
#define ILI9341_PIN_DC   20
#endif
#ifndef ILI9341_PIN_RST
#define ILI9341_PIN_RST  21
#endif
#ifndef ILI9341_PIN_BL
#define ILI9341_PIN_BL   22
#endif

#define ILI9341_WIDTH    320
#define ILI9341_HEIGHT   240

/* Basic LCD controls */
void ili9341_init(void);
void ili9341_set_backlight(bool on);
void ili9341_set_rotation(uint8_t rotation);
void ili9341_clear_black(void);

/* Framebuffer access (RGB565, 320x240) */
void ili9341_set_framebuffer(uint16_t *fb);
uint16_t *ili9341_get_framebuffer(void);
uint16_t *ili9341_get_draw_buffer(void);

/* Synchronous / Direct flush */
void ili9341_flush(bool wait);
void ili9341_flush_rows(int y_start, int y_end);
void ili9341_flush_buffer(const uint16_t *buffer, bool wait);

/* Multicore (Core 1) background display pipeline */
void ili9341_core1_start(void);
void ili9341_submit_frame(void);
void ili9341_wait_frame_done(void);
bool ili9341_is_frame_busy(void);

/* Per-scanline streaming blitter (interlaced full-screen scaling on Core 1) */
void ili9341_stream_nes_line(int line, const uint16_t *rgb555_256);
void ili9341_stream_nes_frame_end(void);
void ili9341_pce_set_geometry(int width, int height);
void ili9341_stream_pce_line(int line, const uint8_t *indexed_pce, const uint16_t *palette_rgb565);
void ili9341_stream_pce_frame_end(void);
/* Sample the DMG bezel window state for the scanline path.
 *
 * Call once per frame. The blitter resolves the window geometry from the sample
 * rather than querying /boot.txt per scanline. */
void ili9341_sample_gb_mode(void);

void ili9341_md_set_geometry(int width, int height);
bool ili9341_is_active_md_line(int line, int height);
void ili9341_stream_md_line(int line, const uint16_t *rgb565_line, int width, int height);
void ili9341_stream_md_frame_end(void);
void ili9341_stream_snes_line(int line, const uint16_t *rgb555_line, int width, int height);

/* Whole-frame pixel accounting, counted before any scaling or filtering.
 * Tells "the core rasterised nothing" apart from "this driver lost it". */
uint32_t ili9341_frame_nonzero(void);
uint32_t ili9341_frame_lines(void);
uint32_t ili9341_frame_checksum(void);
void     ili9341_frame_reset(void);

/* Core 1 -> panel accounting, counted on the side that actually talks to SPI.
 *
 * ili9341_frame_nonzero() is sampled upstream in ili9341_stream_snes_line(), so it
 * reports a healthy core even when nothing reaches the glass. These are read from
 * the Core 0 diagnostic and reset from there, so they cover a frame window:
 *
 *   rows        rows pulled out of the slot queue and passed to the panel
 *   nonzero     non-black pixels in those rows, i.e. 0 = the queue delivered black
 *   row_changes how often the row index changed; flat means the blitter is
 *               painting one row forever instead of sweeping the panel
 *
 * rows > 0 with nonzero == 0 means the queue was drained and the panel was fed
 * black. rows == 0 means snes_band_push() never queued anything, and the defect
 * is upstream of the queue. */
void ili9341_c1_stats(uint32_t *rows, uint32_t *nonzero, uint32_t *row_changes);
void ili9341_c1_stats_reset(void);
/* Time Core 0 spent waiting for a free blitter slot (SPI-bound frames). */
uint32_t ili9341_core0_wait_us(void);
void ili9341_core0_wait_reset(void);
void ili9341_stream_snes_frame_end(void);
void ili9341_snes_set_source_height(int source_h);
void ili9341_snes_get_ymap(int source_line, uint8_t *first_row, uint8_t *last_row);
void ili9341_blitter_start(void);
void ili9341_blitter_stop(void);
bool ili9341_is_blitter_active(void);
void ili9341_set_interleave(bool enable);
bool ili9341_get_interleave(void);
uint8_t ili9341_get_field_parity(void);
bool ili9341_snes_line_active(int line);
void ili9341_set_scanlines(bool enable);
bool ili9341_get_scanlines(void);
void ili9341_set_fps_overlay(bool enable, const char *fps_str);
bool ili9341_get_fps_overlay(void);

/* Scale modes */
typedef enum {
    SCALE_FIT = 0,     /* Proportional 4:3 centered */
    SCALE_STRETCH = 1, /* Full screen 320x240 stretched */
    SCALE_INTEGER = 2  /* 1:1 Pixel Native centered */
} ili9341_scale_mode_t;

void ili9341_set_scale_mode(ili9341_scale_mode_t mode);
ili9341_scale_mode_t ili9341_get_scale_mode(void);

/* Graphics and UI primitives */
void ili9341_fill_screen(uint16_t color);
void ili9341_fill_rect(int x, int y, int w, int h, uint16_t color);
void ili9341_write_rect_565(int x, int y, int w, int h, const uint16_t *px);
void ili9341_draw_char(char c, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg);
void ili9341_text(const char *str, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg);

#ifdef __cplusplus
}
#endif

#endif /* ILI9341_H */
