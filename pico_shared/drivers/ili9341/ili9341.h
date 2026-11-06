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

/* Per-scanline streaming blitter (interlaced full-screen scaling on Core 1) */
void ili9341_stream_nes_line(int line, const uint16_t *rgb555_256);
void ili9341_stream_nes_frame_end(void);
void ili9341_pce_set_geometry(int width, int height);
void ili9341_stream_pce_line(int line, const uint8_t *indexed_pce, const uint16_t *palette_rgb565);
void ili9341_stream_pce_frame_end(void);
void ili9341_md_set_geometry(int width, int height);
bool ili9341_is_active_md_line(int line, int height);
void ili9341_stream_md_line(int line, const uint16_t *rgb565_line, int width, int height);
void ili9341_stream_md_frame_end(void);
void ili9341_stream_ngp_line(int line, const uint16_t *ngp_line_160);
bool ili9341_is_active_ngp_line(int line);
void ili9341_stream_ngp_frame_end(void);

/* WonderSwan streaming (224x144 8-bit indexed, ws_shades RGB888 palette) */
void ili9341_stream_ws_line(int line, const uint8_t *indexed_224, const uint32_t *ws_shades_rgb888);
bool ili9341_is_active_ws_line(int line);
void ili9341_stream_ws_vline(int vline, const uint8_t *indexed_144, const uint32_t *ws_shades_rgb888);
void ili9341_set_ws_vertical(bool vertical);
bool ili9341_get_ws_vertical(void);
void ili9341_stream_ws_frame_end(void);
void ili9341_invalidate_ws_palette(void);
void ili9341_ws_palette_changed(uint32_t bank);

/* WonderSwan display profiling (all times in microseconds, one-second window) */
typedef struct {
    uint32_t rows_pushed;   /* rows handed to Core 1 */
    uint32_t rows_skipped;  /* rows dropped by the dirty-row hash */
    uint32_t push_us;       /* total time in ws_band_push, spin included */
    uint32_t spin_us;       /* Core 0 waiting for a free slot */
    uint32_t c1_us;         /* Core 1 busy streaming rows over SPI */
    uint32_t epoch_bumps;   /* palette-change notifications */
} ili9341_ws_prof_t;
void ili9341_ws_prof_take(ili9341_ws_prof_t *out);

void ili9341_set_grid_effect(bool enable);
bool ili9341_get_grid_effect(void);
void ili9341_set_interleave(bool enable);
bool ili9341_get_interleave(void);
void ili9341_blitter_start(void);
void ili9341_blitter_stop(void);
void ili9341_set_scanlines(bool enable);
bool ili9341_get_scanlines(void);
void ili9341_set_shimmerless(bool enable);
bool ili9341_get_shimmerless(void);
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
