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

/* Core 1 background task hook (e.g. audio offload) */
typedef void (*ili9341_task_fn)(void);
void ili9341_set_background_task(ili9341_task_fn task);

/* Per-scanline streaming blitter (interlaced full-screen scaling on Core 1) */
void ili9341_stream_nes_line(int line, const uint16_t *rgb555_256);
void ili9341_stream_nes_frame_end(void);
void ili9341_pce_set_geometry(int width, int height);
void ili9341_stream_pce_line(int line, const uint8_t *indexed_pce, const uint16_t *palette_rgb565);
void ili9341_stream_pce_frame_end(void);
void ili9341_md_set_geometry(int width, int height);
bool ili9341_is_active_md_line(int line, int height);
extern uint8_t md_line_active[240];
extern uint8_t md_line_dst_row[240];
void md_update_interleave_field(void);
void ili9341_stream_md_line(int line, const uint16_t *rgb565_line, int width, int height);
void ili9341_stream_md_line_fast(uint8_t row, const uint16_t *rgb565_line);
void ili9341_stream_md_frame_end(void);
void ili9341_blitter_start(void);
void ili9341_blitter_stop(void);
void ili9341_set_scanlines(bool enable);
bool ili9341_get_scanlines(void);
void ili9341_set_fps_overlay(bool enable, const char *fps_str);
bool ili9341_get_fps_overlay(void);
void ili9341_set_interleave(bool enable);
bool ili9341_get_interleave(void);
void ili9341_set_shimmerless(bool enable);
bool ili9341_get_shimmerless(void);

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
