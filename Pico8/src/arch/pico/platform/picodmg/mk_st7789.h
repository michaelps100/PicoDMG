#ifndef MK_ST7789_H
#define MK_ST7789_H

#include <stdint.h>
#include <stdbool.h>
#include "mk_ili9225.h"
#include "dmg_menu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ST7789 / ILI9341 display hardware with boot.txt options */
unsigned mk_ili9225_init(void);

/* Scale mode controls */
void mk_ili9225_set_scale_mode(ili9341_scale_mode_t mode);
ili9341_scale_mode_t mk_ili9225_get_scale_mode(void);

/* Clean Light UI Primitives for shared_menu */
void mk_ili9225_fill(uint16_t color);
void mk_ili9225_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void mk_ili9225_write_rect_565(int x, int y, int w, int h, const uint16_t *px);
void mk_ili9225_text(char *s, uint16_t x, uint16_t y, uint16_t color, uint16_t bgcolor);

/* Fast DMA frame blit for Celeste PICO-8 (128x128 8-bit paletted) */
void mk_st7789_draw_celeste_frame(const uint8_t *src_128x128, const uint16_t *pal_16, uint8_t fps_val);

/* Fast DMA frame blit for PICO-8 (128x128 4-bit nibbles / 8192 bytes) */
void mk_st7789_draw_pico8_frame(const uint8_t *fb_4bit_128x128, const uint16_t *pal_16, uint8_t fps_val);

/* Backlight brightness control (1..5) */
void mk_st7789_set_brightness(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* MK_ST7789_H */
