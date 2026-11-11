#ifndef DMG_DISPLAY_H
#define DMG_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Display adapter interface for Clean Light UI.
 * Allows shared_menu to render on any display driver (ILI9341, ILI9225, etc.)
 */
typedef struct {
    void (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*draw_text)(const char *text, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg);
    void (*flush_rows)(int y_start, int y_end);
    void (*fill_screen)(uint16_t color);
    void (*flush)(bool wait);
    void (*stop_blitter)(void);
    void (*start_blitter)(void);
    /* Optional. Writes a w x h block of host-order RGB565 straight to the
     * panel, bypassing any framebuffer. Used by the standby-resume intro
     * (dmg_boot_intro.c), which runs on core 1 and sends one column at a time
     * (w = 1) along the panel's scan axis. NULL disables the intro. */
    void (*write_rect_565)(int x, int y, int w, int h, const uint16_t *px);
} dmg_display_t;

#ifdef __cplusplus
}
#endif

#endif /* DMG_DISPLAY_H */

