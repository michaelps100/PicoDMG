#pragma GCC optimize("Ofast")
////////////////////////////////////////////////////////////////////////////////
// GPU
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
// 7.04.2002: Fixed sprites order
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////

//#define STATISTICS

#include <string.h>
#include "pico.h"
#include <stdbool.h>
#include <io.h>
#include "./nec/nec.h"
#include "io.h"
#include "gpu.h"

/* Declared here rather than via an include: src/oswan does not carry
 * pico_shared/drivers on its include path, and this is the only symbol the GPU
 * needs from the display driver. Declared in ili9341.h as well. */
void ili9341_ws_palette_changed(uint32 bank);

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
#if 0
extern uint8	*internalRam;
#else
extern uint8 internalRam[0x10000];
#endif

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
#ifdef STATISTICS
                                                                                                                        long	ws_4_shades_tiles_cache_update_time=0;
	long	ws_4_colors_tiles_cache_update_time=0;
	long	ws_16_colors_packed_tiles_cache_update_time=0;
	long	ws_16_colors_layered_tiles_cache_update_time=0;

	long	ws_4_shades_tiles_cache_update_number=0;
	long	ws_4_colors_tiles_cache_update_number=0;
	long	ws_16_colors_packed_tiles_cache_update_number=0;
	long	ws_16_colors_layered_tiles_cache_update_number=0;

	long	ws_background_color_rendering_time=0;
	long	ws_background_rendering_time=0;
	long	ws_foreground_rendering_time=0;
	long	ws_priority_0_sprites_rendering_time=0;
	long	ws_priority_1_sprites_rendering_time=0;
#endif
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
#define RGB(r, g, b) (((int)r<<16) | ((int)g << 8 ) | (int)b )

uint8 ws_gpu_operatingInColor;
/* What the renderer (and the display driver) should draw: colour tiles and the 256-entry
 * palette only while the console is a WonderSwan Color AND the game has switched the video
 * hardware to colour (bit 7 of port 0x60). A colour system running a game in its mono
 * compatibility mode -- Final Fantasy, after "New Game" -- has to be drawn with the mono
 * path: the colour path has no decoder for video mode 0, so its tiles stayed unconverted
 * and the screen filled with the backdrop colour and stale glyphs. */
uint8 ws_gpu_colorActive;
uint8 ws_videoMode;
uint8 ws_gpu_scanline = 0;
__aligned(4) int16 ws_palette[16 * 4];
__aligned(4) int8 ws_paletteColors[8];
__aligned(4) int16 wsc_palette[16 * 16];
__aligned(4) uint32 ws_shades[16];
__aligned(4) uint16_t wsc_palette_rgb565[256];
/* Same colours in native byte order: the display blend wants them unswapped */
__aligned(4) uint16_t wsc_palette_rgb565_native[256];
int ws_gpu_forceColorSystemBool = 0;
int ws_gpu_forceMonoSystemBool = 0;



// white
#define SHADE_COLOR_RED        8.50
#define SHADE_COLOR_GREEN    8.50
#define SHADE_COLOR_BLUE    8.50

const uint32 ws_colour_scheme_default[16] = {
        RGB(SHADE_COLOR_RED * 30, SHADE_COLOR_GREEN * 30, SHADE_COLOR_BLUE * 30),
        RGB(SHADE_COLOR_RED * 28, SHADE_COLOR_GREEN * 28, SHADE_COLOR_BLUE * 28),
        RGB(SHADE_COLOR_RED * 26, SHADE_COLOR_GREEN * 26, SHADE_COLOR_BLUE * 26),
        RGB(SHADE_COLOR_RED * 24, SHADE_COLOR_GREEN * 24, SHADE_COLOR_BLUE * 24),
        RGB(SHADE_COLOR_RED * 22, SHADE_COLOR_GREEN * 22, SHADE_COLOR_BLUE * 22),
        RGB(SHADE_COLOR_RED * 20, SHADE_COLOR_GREEN * 20, SHADE_COLOR_BLUE * 20),
        RGB(SHADE_COLOR_RED * 18, SHADE_COLOR_GREEN * 18, SHADE_COLOR_BLUE * 18),
        RGB(SHADE_COLOR_RED * 16, SHADE_COLOR_GREEN * 16, SHADE_COLOR_BLUE * 16),
        RGB(SHADE_COLOR_RED * 14, SHADE_COLOR_GREEN * 14, SHADE_COLOR_BLUE * 14),
        RGB(SHADE_COLOR_RED * 12, SHADE_COLOR_GREEN * 12, SHADE_COLOR_BLUE * 12),
        RGB(SHADE_COLOR_RED * 10, SHADE_COLOR_GREEN * 10, SHADE_COLOR_BLUE * 10),
        RGB(SHADE_COLOR_RED * 8, SHADE_COLOR_GREEN * 8, SHADE_COLOR_BLUE * 8),
        RGB(SHADE_COLOR_RED * 6, SHADE_COLOR_GREEN * 6, SHADE_COLOR_BLUE * 6),
        RGB(SHADE_COLOR_RED * 4, SHADE_COLOR_GREEN * 4, SHADE_COLOR_BLUE * 4),
        RGB(SHADE_COLOR_RED * 2, SHADE_COLOR_GREEN * 2, SHADE_COLOR_BLUE * 2),
        RGB(SHADE_COLOR_RED * 0, SHADE_COLOR_GREEN * 0, SHADE_COLOR_BLUE * 0)
};
// green
#undef  SHADE_COLOR_RED
#undef  SHADE_COLOR_GREEN
#undef  SHADE_COLOR_BLUE
#define SHADE_COLOR_RED        1.70
#define SHADE_COLOR_GREEN    7.65
#define SHADE_COLOR_BLUE    1.70

const uint32 ws_colour_scheme_green[16] = {
        RGB(SHADE_COLOR_RED * 30, SHADE_COLOR_GREEN * 30, SHADE_COLOR_BLUE * 30),
        RGB(SHADE_COLOR_RED * 28, SHADE_COLOR_GREEN * 28, SHADE_COLOR_BLUE * 28),
        RGB(SHADE_COLOR_RED * 26, SHADE_COLOR_GREEN * 26, SHADE_COLOR_BLUE * 26),
        RGB(SHADE_COLOR_RED * 24, SHADE_COLOR_GREEN * 24, SHADE_COLOR_BLUE * 24),
        RGB(SHADE_COLOR_RED * 22, SHADE_COLOR_GREEN * 22, SHADE_COLOR_BLUE * 22),
        RGB(SHADE_COLOR_RED * 20, SHADE_COLOR_GREEN * 20, SHADE_COLOR_BLUE * 20),
        RGB(SHADE_COLOR_RED * 18, SHADE_COLOR_GREEN * 18, SHADE_COLOR_BLUE * 18),
        RGB(SHADE_COLOR_RED * 16, SHADE_COLOR_GREEN * 16, SHADE_COLOR_BLUE * 16),
        RGB(SHADE_COLOR_RED * 14, SHADE_COLOR_GREEN * 14, SHADE_COLOR_BLUE * 14),
        RGB(SHADE_COLOR_RED * 12, SHADE_COLOR_GREEN * 12, SHADE_COLOR_BLUE * 12),
        RGB(SHADE_COLOR_RED * 10, SHADE_COLOR_GREEN * 10, SHADE_COLOR_BLUE * 10),
        RGB(SHADE_COLOR_RED * 8, SHADE_COLOR_GREEN * 8, SHADE_COLOR_BLUE * 8),
        RGB(SHADE_COLOR_RED * 6, SHADE_COLOR_GREEN * 6, SHADE_COLOR_BLUE * 6),
        RGB(SHADE_COLOR_RED * 4, SHADE_COLOR_GREEN * 4, SHADE_COLOR_BLUE * 4),
        RGB(SHADE_COLOR_RED * 2, SHADE_COLOR_GREEN * 2, SHADE_COLOR_BLUE * 2),
        RGB(SHADE_COLOR_RED * 0, SHADE_COLOR_GREEN * 0, SHADE_COLOR_BLUE * 0)
};
// amber
#undef  SHADE_COLOR_RED
#undef  SHADE_COLOR_GREEN
#undef  SHADE_COLOR_BLUE
#define SHADE_COLOR_RED        8.50
#define SHADE_COLOR_GREEN    5.20
#define SHADE_COLOR_BLUE    0.00

const uint32 ws_colour_scheme_amber[16] = {
        RGB(SHADE_COLOR_RED * 30, SHADE_COLOR_GREEN * 30, SHADE_COLOR_BLUE * 30),
        RGB(SHADE_COLOR_RED * 28, SHADE_COLOR_GREEN * 28, SHADE_COLOR_BLUE * 28),
        RGB(SHADE_COLOR_RED * 26, SHADE_COLOR_GREEN * 26, SHADE_COLOR_BLUE * 26),
        RGB(SHADE_COLOR_RED * 24, SHADE_COLOR_GREEN * 24, SHADE_COLOR_BLUE * 24),
        RGB(SHADE_COLOR_RED * 22, SHADE_COLOR_GREEN * 22, SHADE_COLOR_BLUE * 22),
        RGB(SHADE_COLOR_RED * 20, SHADE_COLOR_GREEN * 20, SHADE_COLOR_BLUE * 20),
        RGB(SHADE_COLOR_RED * 18, SHADE_COLOR_GREEN * 18, SHADE_COLOR_BLUE * 18),
        RGB(SHADE_COLOR_RED * 16, SHADE_COLOR_GREEN * 16, SHADE_COLOR_BLUE * 16),
        RGB(SHADE_COLOR_RED * 14, SHADE_COLOR_GREEN * 14, SHADE_COLOR_BLUE * 14),
        RGB(SHADE_COLOR_RED * 12, SHADE_COLOR_GREEN * 12, SHADE_COLOR_BLUE * 12),
        RGB(SHADE_COLOR_RED * 10, SHADE_COLOR_GREEN * 10, SHADE_COLOR_BLUE * 10),
        RGB(SHADE_COLOR_RED * 8, SHADE_COLOR_GREEN * 8, SHADE_COLOR_BLUE * 8),
        RGB(SHADE_COLOR_RED * 6, SHADE_COLOR_GREEN * 6, SHADE_COLOR_BLUE * 6),
        RGB(SHADE_COLOR_RED * 4, SHADE_COLOR_GREEN * 4, SHADE_COLOR_BLUE * 4),
        RGB(SHADE_COLOR_RED * 2, SHADE_COLOR_GREEN * 2, SHADE_COLOR_BLUE * 2),
        RGB(SHADE_COLOR_RED * 0, SHADE_COLOR_GREEN * 0, SHADE_COLOR_BLUE * 0)
};
#if 0
                                                                                                                        uint8	*ws_tile_cache;
uint8	*ws_hflipped_tile_cache;

uint8	*wsc_tile_cache;
uint8	*wsc_hflipped_tile_cache;

uint8	*ws_modified_tile;
uint8	*wsc_modified_tile;
#else
__aligned(4) uint8 wsc_tile_cache[1024 * 8 * 8];
uint8 *ws_tile_cache = wsc_tile_cache;
__aligned(4) uint8 wsc_modified_tile[1024];
uint8 *ws_modified_tile = wsc_modified_tile;

#endif

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_set_colour_scheme(int scheme) {
    switch (scheme) {
        case COLOUR_SCHEME_DEFAULT:
            memcpy(ws_shades, ws_colour_scheme_default, 16 * sizeof(uint32));
            break;
        case COLOUR_SCHEME_AMBER  :
            memcpy(ws_shades, ws_colour_scheme_amber, 16 * sizeof(uint32));
            break;
        case COLOUR_SCHEME_GREEN  :
            memcpy(ws_shades, ws_colour_scheme_green, 16 * sizeof(uint32));
            break;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_forceColorSystem(void) {
    ws_gpu_forceColorSystemBool = 1;
    ws_gpu_forceMonoSystemBool = 0;
    ws_gpu_operatingInColor = 1;
    ws_gpu_colorActive = (ws_ioRam[0x60] & 0x80) ? 1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_forceMonoSystem(void) {
    ws_gpu_forceColorSystemBool = 0;
    ws_gpu_forceMonoSystemBool = 1;
    ws_gpu_operatingInColor = 0;
    ws_gpu_colorActive = 0;
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_init(void) {
    memset(wsc_tile_cache, 0x00, 1024 * 8 * 8);
    memset(wsc_modified_tile, 0x01, 1024);

    ws_gpu_forceColorSystemBool = 0;
    ws_gpu_forceMonoSystemBool = 0;
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_done(void) {
#if 0
                                                                                                                            free(ws_tile_cache);
	free(wsc_tile_cache);
	free(ws_hflipped_tile_cache);
	free(wsc_hflipped_tile_cache);
	free(ws_modified_tile);
	free(wsc_modified_tile);
#endif
#ifdef STATISTICS
                                                                                                                            printf("Statistics:\n");
	printf("\tcache:\n");
	if (ws_4_shades_tiles_cache_update_number)
		printf("\t\t4 shades tiles update time         : %i\n",ws_4_shades_tiles_cache_update_time/ws_4_shades_tiles_cache_update_number);
	if (ws_4_colors_tiles_cache_update_number)
		printf("\t\t4 colors tiles update time         : %i\n",ws_4_colors_tiles_cache_update_time/ws_4_colors_tiles_cache_update_number);
	if (ws_16_colors_packed_tiles_cache_update_number)
		printf("\t\t16 colors packed tiles update time : %i\n",ws_16_colors_packed_tiles_cache_update_time/ws_16_colors_packed_tiles_cache_update_number);
	if (ws_16_colors_layered_tiles_cache_update_number)
		printf("\t\t16 colors layered tiles update time: %i\n",ws_16_colors_layered_tiles_cache_update_time/ws_16_colors_layered_tiles_cache_update_number);

	printf("\tscanline rendering:\n");
	long	total=	ws_background_color_rendering_time+ws_background_rendering_time+
					ws_foreground_rendering_time+ws_priority_0_sprites_rendering_time+
					ws_priority_1_sprites_rendering_time;
	printf("\t\tbackground color   : %4i (%3i %%)\n",ws_background_color_rendering_time,(ws_background_color_rendering_time*100)/total);
	printf("\t\tbackground         : %4i (%3i %%)\n",ws_background_rendering_time,(ws_background_rendering_time*100)/total);
	printf("\t\tforeground         : %4i (%3i %%)\n",ws_foreground_rendering_time,(ws_foreground_rendering_time*100)/total);
	printf("\t\tpriority 0 sprites : %4i (%3i %%)\n",ws_priority_0_sprites_rendering_time,(ws_priority_0_sprites_rendering_time*100)/total);
	printf("\t\tpriority 1 sprites : %4i (%3i %%)\n",ws_priority_1_sprites_rendering_time,(ws_priority_1_sprites_rendering_time*100)/total);
#endif
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_changeVideoMode(uint8 value) {
    ws_gpu_colorActive = (ws_gpu_operatingInColor && (value & 0x80)) ? 1 : 0;
    if (ws_videoMode != (value >> 5)) {
        ws_videoMode = value >> 5;
        if (ws_videoMode == 4)
            ws_videoMode = 2;

        // mark all tiles dirty
        memset(wsc_modified_tile, 0x01, 1024);
        //memset(ws_modified_tile, 0x01, 512);
    }
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_reset(void) {
    memset(wsc_modified_tile, 0x01, 1024);
    //memset(ws_modified_tile, 0x01, 512);
    ws_gpu_scanline = 0;
    ws_gpu_changeVideoMode(2);
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_gpu_clearCache(void) {
    memset(wsc_modified_tile, 0x01, 1024);
    //memset(ws_modified_tile, 0x01, 512);
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
static __always_inline uint8 *ws_tileCache_getTileRow(uint32 tileIndex, uint32 line,
                                                      uint32 vFlip, uint32 hFlip, uint32 bank) {
    if (ws_gpu_colorActive) {
        if (bank)
            tileIndex += 512;

        // need to update tile cache ?
        // 4 colors tiles
        if ((ws_videoMode == 2) && (ws_modified_tile[tileIndex])) {
#ifdef STATISTICS
            ws_4_colors_tiles_cache_update_time+=-ticker();
#endif
            uint8 *tileInCachePtr = &wsc_tile_cache[tileIndex << 6];
            uint16 *tileInRamPtr = (uint16 *) &internalRam[0x2000 + (tileIndex << 4)];
            uint16 tileLine;

            for (int line = 0; line < 8; line++) {
                tileLine = *tileInRamPtr++;

                tileInCachePtr[0] = ((tileLine & 0x80) >> 7) | ((tileLine & 0x8000) >> 14);
                tileInCachePtr[1] = ((tileLine & 0x40) >> 6) | ((tileLine & 0x4000) >> 13);
                tileInCachePtr[2] = ((tileLine & 0x20) >> 5) | ((tileLine & 0x2000) >> 12);
                tileInCachePtr[3] = ((tileLine & 0x10) >> 4) | ((tileLine & 0x1000) >> 11);
                tileInCachePtr[4] = ((tileLine & 0x08) >> 3) | ((tileLine & 0x0800) >> 10);
                tileInCachePtr[5] = ((tileLine & 0x04) >> 2) | ((tileLine & 0x0400) >> 9);
                tileInCachePtr[6] = ((tileLine & 0x02) >> 1) | ((tileLine & 0x0200) >> 8);
                tileInCachePtr[7] = ((tileLine & 0x01) >> 0) | ((tileLine & 0x0100) >> 7);

                tileInCachePtr += 8;
            }
            ws_modified_tile[tileIndex] = 0;
#ifdef STATISTICS
            ws_4_colors_tiles_cache_update_time+=ticker();
			ws_4_colors_tiles_cache_update_number++;
#endif
        } else if (wsc_modified_tile[tileIndex]) {
            // 16 colors by tile layered mode
            if (ws_videoMode == 6) {
#ifdef STATISTICS
                ws_16_colors_layered_tiles_cache_update_time+=-ticker();
#endif
                uint8 *tileInCachePtr = &wsc_tile_cache[tileIndex << 6];
                uint32 *tileInRamPtr = (uint32 *) &internalRam[0x4000 + (tileIndex << 5)];
                uint32 tileLine;

#pragma GCC unroll 8
                for (int line = 0; line < 8; line++) {
                    tileLine = *tileInRamPtr++;

                    tileInCachePtr[0] = ((tileLine & 0x00000080) >> 7) | ((tileLine & 0x00008000) >> 14) |
                                        ((tileLine & 0x00800000) >> 21) | ((tileLine & 0x80000000) >> 28);
                    tileInCachePtr[1] = ((tileLine & 0x00000040) >> 6) | ((tileLine & 0x00004000) >> 13) |
                                        ((tileLine & 0x00400000) >> 20) | ((tileLine & 0x40000000) >> 27);
                    tileInCachePtr[2] = ((tileLine & 0x00000020) >> 5) | ((tileLine & 0x00002000) >> 12) |
                                        ((tileLine & 0x00200000) >> 19) | ((tileLine & 0x20000000) >> 26);
                    tileInCachePtr[3] = ((tileLine & 0x00000010) >> 4) | ((tileLine & 0x00001000) >> 11) |
                                        ((tileLine & 0x00100000) >> 18) | ((tileLine & 0x10000000) >> 25);
                    tileInCachePtr[4] = ((tileLine & 0x00000008) >> 3) | ((tileLine & 0x00000800) >> 10) |
                                        ((tileLine & 0x00080000) >> 17) | ((tileLine & 0x08000000) >> 24);
                    tileInCachePtr[5] = ((tileLine & 0x00000004) >> 2) | ((tileLine & 0x00000400) >> 9) |
                                        ((tileLine & 0x00040000) >> 16) | ((tileLine & 0x04000000) >> 23);
                    tileInCachePtr[6] = ((tileLine & 0x00000002) >> 1) | ((tileLine & 0x00000200) >> 8) |
                                        ((tileLine & 0x00020000) >> 15) | ((tileLine & 0x02000000) >> 22);
                    tileInCachePtr[7] = ((tileLine & 0x00000001) >> 0) | ((tileLine & 0x00000100) >> 7) |
                                        ((tileLine & 0x00010000) >> 14) | ((tileLine & 0x01000000) >> 21);

                    tileInCachePtr += 8;
                }
#ifdef STATISTICS
                ws_16_colors_layered_tiles_cache_update_time+=ticker();
				ws_16_colors_layered_tiles_cache_update_number++;
#endif
            } else
                // 16 colors by tile packed mode
            if (ws_videoMode == 7) {
#ifdef STATISTICS
                ws_16_colors_packed_tiles_cache_update_time+=-ticker();
#endif
                uint8 *tileInCachePtr = &wsc_tile_cache[tileIndex << 6];
                uint32 *tileInRamPtr = (uint32 *) &internalRam[0x4000 + (tileIndex << 5)];
                uint32 tileLine;

#pragma GCC unroll 8
                for (int line = 0; line < 8; line++) {
                    tileLine = *tileInRamPtr++;

                    tileInCachePtr[0] = (tileLine >> 4) & 0x0f;
                    tileInCachePtr[1] = (tileLine >> 0) & 0x0f;
                    tileInCachePtr[2] = (tileLine >> 12) & 0x0f;
                    tileInCachePtr[3] = (tileLine >> 8) & 0x0f;
                    tileInCachePtr[4] = (tileLine >> 20) & 0x0f;
                    tileInCachePtr[5] = (tileLine >> 16) & 0x0f;
                    tileInCachePtr[6] = (tileLine >> 28) & 0x0f;
                    tileInCachePtr[7] = (tileLine >> 24) & 0x0f;

                    tileInCachePtr += 8;
                }
#ifdef STATISTICS
                ws_16_colors_packed_tiles_cache_update_time+=ticker();
				ws_16_colors_packed_tiles_cache_update_number++;
#endif
            } else {
                // unknown mode
            }
            // tile cache updated
            wsc_modified_tile[tileIndex] = 0;
        }
        if (vFlip)
            line = 7 - line;
        if (hFlip) {
            static uint8 s_hflip[8];
            const uint8 *src = &wsc_tile_cache[(tileIndex << 6) + (line << 3)];
            s_hflip[0] = src[7];
            s_hflip[1] = src[6];
            s_hflip[2] = src[5];
            s_hflip[3] = src[4];
            s_hflip[4] = src[3];
            s_hflip[5] = src[2];
            s_hflip[6] = src[1];
            s_hflip[7] = src[0];
            return s_hflip;
        } else
            return (&wsc_tile_cache[(tileIndex << 6) + (line << 3)]);

    } else {
        // need to update tile cache ?
        if (ws_modified_tile[tileIndex]) {
#ifdef STATISTICS
            ws_4_shades_tiles_cache_update_time+=-ticker();
#endif
            uint8 *tileInCachePtr = &ws_tile_cache[tileIndex << 6];
            uint32 *tileInRamPtr = (uint32 *) &internalRam[0x2000 + (tileIndex << 4)];
            uint32 tileLine;

#pragma GCC unroll 4
            for (int line = 0; line < 4; line++) {
                tileLine = *tileInRamPtr++;

                *tileInCachePtr++ = ((tileLine & 0x80) >> 7) | ((tileLine & 0x8000) >> 14);
                *tileInCachePtr++ = ((tileLine & 0x40) >> 6) | ((tileLine & 0x4000) >> 13);
                *tileInCachePtr++ = ((tileLine & 0x20) >> 5) | ((tileLine & 0x2000) >> 12);
                *tileInCachePtr++ = ((tileLine & 0x10) >> 4) | ((tileLine & 0x1000) >> 11);
                *tileInCachePtr++ = ((tileLine & 0x08) >> 3) | ((tileLine & 0x0800) >> 10);
                *tileInCachePtr++ = ((tileLine & 0x04) >> 2) | ((tileLine & 0x0400) >> 9);
                *tileInCachePtr++ = ((tileLine & 0x02) >> 1) | ((tileLine & 0x0200) >> 8);
                *tileInCachePtr++ = ((tileLine & 0x01) >> 0) | ((tileLine & 0x0100) >> 7);
                tileLine >>= 16;
                *tileInCachePtr++ = ((tileLine & 0x80) >> 7) | ((tileLine & 0x8000) >> 14);
                *tileInCachePtr++ = ((tileLine & 0x40) >> 6) | ((tileLine & 0x4000) >> 13);
                *tileInCachePtr++ = ((tileLine & 0x20) >> 5) | ((tileLine & 0x2000) >> 12);
                *tileInCachePtr++ = ((tileLine & 0x10) >> 4) | ((tileLine & 0x1000) >> 11);
                *tileInCachePtr++ = ((tileLine & 0x08) >> 3) | ((tileLine & 0x0800) >> 10);
                *tileInCachePtr++ = ((tileLine & 0x04) >> 2) | ((tileLine & 0x0400) >> 9);
                *tileInCachePtr++ = ((tileLine & 0x02) >> 1) | ((tileLine & 0x0200) >> 8);
                *tileInCachePtr++ = ((tileLine & 0x01) >> 0) | ((tileLine & 0x0100) >> 7);
            }
            // tile cache updated
            ws_modified_tile[tileIndex] = 0;
#ifdef STATISTICS
            ws_4_shades_tiles_cache_update_time+=ticker();
			ws_4_shades_tiles_cache_update_number++;
#endif
        }
        if (vFlip)
            line = 7 - line;
        if (hFlip) {
            static uint8 s_hflip_mono[8];
            const uint8 *src = &ws_tile_cache[(tileIndex << 6) + (line << 3)];
            s_hflip_mono[0] = src[7];
            s_hflip_mono[1] = src[6];
            s_hflip_mono[2] = src[5];
            s_hflip_mono[3] = src[4];
            s_hflip_mono[4] = src[3];
            s_hflip_mono[5] = src[2];
            s_hflip_mono[6] = src[1];
            s_hflip_mono[7] = src[0];
            return s_hflip_mono;
        } else
            return (&ws_tile_cache[(tileIndex << 6) + (line << 3)]);
    }
    return (NULL);
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
static __always_inline void ws_drawClippedSpriteLine(uint8 *framebuffer, uint16 scanline,
                                     uint32 x, uint32 y, uint32 tileIndex, uint32 paletteIndex,
                                     uint32 vFlip, uint32 hFlip,
                                     uint32 clip_x0, uint32 clip_y0, uint32 clip_x1, uint32 clip_y1) {

    if ((scanline < y) || (scanline > (y + 7)))
        return;
    if ((x + 7 < clip_x0) || (x >= clip_x1))
        return;
    if ((y + 7 < clip_y0) || (y >= clip_y1))
        return;

    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileIndex, (scanline - y) & 0x07, hFlip, vFlip, 0);
    uint16 nbPixels = 8;
    if (x < clip_x0) {
        ws_tileRow += clip_x0 - x;
        nbPixels -= clip_x0 - x;
        x = clip_x0;
    }
    if (x + nbPixels > clip_x1)
        nbPixels = (clip_x1 - x);
    framebuffer += x;

    if (ws_gpu_colorActive) {
        while (nbPixels) {
            if (*ws_tileRow) *framebuffer = (paletteIndex << 4) + *ws_tileRow;
            framebuffer++;
            ws_tileRow++;
            nbPixels--;
        }
    } else {
        int16 *ws_paletteAlias = &ws_palette[paletteIndex << 2];
        if (paletteIndex & 0x04) {
            while (nbPixels) {
                if (*ws_tileRow) *framebuffer = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                framebuffer++;
                ws_tileRow++;
                nbPixels--;
            }
        } else {
            while (nbPixels) {
                *framebuffer = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                framebuffer++;
                ws_tileRow++;
                nbPixels--;
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
/* Console build only: ~12 KB of SRAM, which the HW15 board does not have to spare */
#if defined(PSRAM_CS_PIN) && PSRAM_CS_PIN
/* -------------------------------------------------------------------------- */
/* SRAM fast path for the common WonderSwan Color scanline                     */
/*                                                                            */
/* ws_gpu_renderScanline() below is 123 KB of unrolled code that lives in flash */
/* and fights the PSRAM ROM for the 16 KB XIP cache. A colour line with no     */
/* foreground window and no sprite window -- almost every game, almost every    */
/* line -- needs only this: background, priority-0 sprites, foreground,         */
/* priority-1 sprites. Same pixels, same order, same transparency rules, but a  */
/* few KB that run from SRAM. Anything else falls through to the full renderer. */
/* -------------------------------------------------------------------------- */
static void __not_in_flash_func(ws_render_layer_color)(uint8 *fb, uint32 mapBase,
                                                       uint32 scrollX, uint32 scrollY, uint32 scanline) {
    scrollY = (scrollY + scanline) & 0xff;
    const uint32 firstTile = scrollX >> 3;
    const uint16 *map = (const uint16 *) (internalRam + mapBase + ((scrollY & 0xfff8) << 3));
    const uint32 lineInTile = scrollY & 0x07;
    const int col = (int) (scrollX & 0x07);
    const int nt = col ? 29 : 28;
    int px = -col;

    for (int t = 0; t < nt; t++, px += 8) {
        const uint16 info = map[(firstTile + (uint32) t) & 0x1f];
        const uint8 *row = ws_tileCache_getTileRow(info & 0x1ff, lineInTile,
                                                   info & 0x8000, info & 0x4000, info & 0x2000);
        const uint8 palbase = (uint8) (((info >> 9) & 0x0f) << 4);
        if (px >= 0 && px + 8 <= 224) {
            uint8 *d = fb + px;
            for (int i = 0; i < 8; i++) {
                const uint8 v = row[i];
                if (v) d[i] = (uint8) (palbase + v);
            }
        } else {
            for (int i = 0; i < 8; i++) {
                const int x = px + i;
                if ((unsigned) x < 224u && row[i]) fb[x] = (uint8) (palbase + row[i]);
            }
        }
    }
}

static void __not_in_flash_func(ws_render_sprites_color)(uint8 *fb, uint32 scanline, uint32 priority) {
    const uint32 first = ws_ioRam[0x05];
    const uint32 count = ws_ioRam[0x06];
    const uint32 *sprites = (const uint32 *) (internalRam + (((uint32) ws_ioRam[0x04]) << 9)) + count - 1;

    for (uint32 i = count; i > first; i--) {
        const uint32 spr = *sprites--;
        if ((spr & 0x2000) != priority) continue;

        const uint32 x = (spr & 0xff000000) >> 24;
        const uint32 y = (spr & 0x00ff0000) >> 16;
        if (scanline < y || scanline > y + 7) continue;
        if (x >= 224 || y >= 144) continue;

        const uint8 *row = ws_tileCache_getTileRow(spr & 0x1ff, (scanline - y) & 0x07,
                                                   spr & 0x8000, spr & 0x4000, 0);
        uint32 n = 8;
        if (x + n > 224) n = 224 - x;
        const uint32 palbase = (8 + ((spr & 0xe00) >> 9)) << 4;
        uint8 *d = fb + x;
        for (uint32 k = 0; k < n; k++) {
            if (row[k]) d[k] = (uint8) (palbase + row[k]);
        }
    }
}

static void __not_in_flash_func(ws_render_scanline_color_fast)(uint8 *fb) {
    const uint32 sl = ws_gpu_scanline;
    const uint8 io0 = ws_ioRam[0x00];

    memset(fb, ws_ioRam[0x01], 224);
    if (io0 & 0x01)
        ws_render_layer_color(fb, ((uint32) ws_ioRam[0x07] & 0x0f) << 11, ws_ioRam[0x10], ws_ioRam[0x11], sl);
    if (io0 & 0x04)
        ws_render_sprites_color(fb, sl, 0);
    if (io0 & 0x02)
        ws_render_layer_color(fb, ((uint32) ws_ioRam[0x07] & 0xf0) << 7, ws_ioRam[0x12], ws_ioRam[0x13], sl);
    if (io0 & 0x04)
        ws_render_sprites_color(fb, sl, 0x2000);
}

#endif

void ws_gpu_renderScanline(uint8 *framebuffer) {

    if (ws_gpu_scanline > 143)
        return;

    /* colour, no foreground window (0x30), no sprite window (0x08) */
#if defined(PSRAM_CS_PIN) && PSRAM_CS_PIN && !defined(WS_NO_FAST_RENDER)
    if (ws_gpu_colorActive && !(ws_ioRam[0x00] & 0x38)) {
        ws_render_scanline_color_fast(framebuffer);
        return;
    }
#endif
#ifdef STATISTICS
    long startTime=ticker();
#endif

    // fill with background color
    uint8 backgroundColor;
    if (ws_gpu_colorActive)
        backgroundColor = ws_ioRam[0x01];
    else
        backgroundColor = ws_paletteColors[ws_ioRam[0x01] & 0x07];

    memset(framebuffer, backgroundColor, 224);

#ifdef STATISTICS
    ws_background_color_rendering_time=ticker();
#endif
    // render background layer
    if (ws_ioRam[0x00] & 0x01) {
        int ws_bgScroll_x = ws_ioRam[0x10];
        int ws_bgScroll_y = ws_ioRam[0x11];

        // seek to the first tile
        ws_bgScroll_y = (ws_bgScroll_y + ws_gpu_scanline) & 0xff;

        // note: byte ordering assumptions!
        int ws_currentTile = (ws_bgScroll_x >> 3);
        uint16 *ws_bgScrollRamBase = (uint16 *) (internalRam + (((uint32) ws_ioRam[0x07] & 0x0f) << 11) +
                                                 ((ws_bgScroll_y & 0xfff8) << 3));

        int lineInTile = ws_bgScroll_y & 0x07;
        int columnInTile = ws_bgScroll_x & 0x07;

        uint8 *scanlinePtr = framebuffer;

        if (ws_gpu_colorActive) {
            // render the first clipped tile
            if (columnInTile) {
                uint16 tileInfo = ws_bgScrollRamBase[ws_currentTile & 0x1f];
                ws_currentTile++;
                uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile, tileInfo & 0x8000,
                                                            tileInfo & 0x4000, tileInfo & 0x2000);
                ws_tileRow += columnInTile;

#pragma GCC unroll 8
                for (int i = columnInTile; i < 8; i++) {
                        if (*ws_tileRow) *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                    scanlinePtr++;
                    ws_tileRow++;
                }
            }

            // render the tiles between them
            int nbTiles = 28;
            if (columnInTile)
                nbTiles = 27;

            for (int i = 0; i < nbTiles; i++) {
                uint16 tileInfo = ws_bgScrollRamBase[ws_currentTile & 0x1f];
                ws_currentTile++;
                uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile, tileInfo & 0x8000,
                                                            tileInfo & 0x4000, tileInfo & 0x2000);

                for (int ii = 0; ii < 8; ii++) {
                        if (*ws_tileRow) *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                    scanlinePtr++;
                    ws_tileRow++;
                }
            }

            // render the last clipped tile
            if (columnInTile) {
                uint16 tileInfo = ws_bgScrollRamBase[ws_currentTile & 0x1f];
                ws_currentTile++;
                uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                            tileInfo & 0x8000, tileInfo & 0x4000, tileInfo & 0x2000);


                for (int i = 0; i < columnInTile; i++) {
                        if (*ws_tileRow) *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                    scanlinePtr++;
                    ws_tileRow++;
                }
            }
        } else // monochrome
        {
            // render the first clipped tile
            if (columnInTile) {
                uint16 tileInfo = ws_bgScrollRamBase[ws_currentTile & 0x1f];
                ws_currentTile++;
                uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                            tileInfo & 0x8000, tileInfo & 0x4000, tileInfo & 0x2000);

                int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];
                ws_tileRow += columnInTile;

                /* WonderSwan transparency applies to SCR1 too: bit 2 of the palette
                 * index (bit 11 of the tile attribute) means colour 0 of that
                 * palette is transparent, for BOTH layers. Do not flatten this to
                 * an unconditional write -- Klonoa's HUD relies on it, and so do
                 * sprites whose colour-0 pixels must show the scene behind. */
                if ((tileInfo >> 9) & 0x04) {
                    for (int i = columnInTile; i < 8; i++) {
                        if (*ws_tileRow)
                            *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                } else {
                    for (int i = columnInTile; i < 8; i++) {
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }
            }
            int nbTiles = 28;
            if (columnInTile)
                nbTiles = 27;

            for (int i = 0; i < nbTiles; i++) {
                uint16 tileInfo = ws_bgScrollRamBase[ws_currentTile & 0x1f];
                ws_currentTile++;
                uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                            tileInfo & 0x8000, tileInfo & 0x4000, tileInfo & 0x2000);
                int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];

                if ((tileInfo >> 9) & 0x04) {
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                } else {
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                    *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                    scanlinePtr++;
                    ws_tileRow++;
                }

            }

            // render the last clipped tile
            if (columnInTile) {
                uint16 tileInfo = ws_bgScrollRamBase[ws_currentTile & 0x1f];
                ws_currentTile++;
                uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                            tileInfo & 0x8000, tileInfo & 0x4000, tileInfo & 0x2000);
                int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];


                if ((tileInfo >> 9) & 0x04) {
                    for (int i = 0; i < columnInTile; i++) {
if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                } else {
                    for (int i = 0; i < columnInTile; i++) {
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }
            }
        }
    }
#ifdef STATISTICS
    ws_background_rendering_time=ticker();
#endif
    // render sprites which are between both layers
    if (ws_ioRam[0x00] & 0x04) {
        int ws_sprWindow_x0 = ws_ioRam[0x0c];
        int ws_sprWindow_y0 = ws_ioRam[0x0d];
        int ws_sprWindow_x1 = ws_ioRam[0x0e];
        int ws_sprWindow_y1 = ws_ioRam[0x0f];
        uint32 *ws_sprRamBase = (uint32 *) (internalRam + (((uint32) ws_ioRam[0x04]) << 9));

        // seek to first sprite
        ws_sprRamBase += ws_ioRam[0x06] - 1;

        for (int i = ws_ioRam[0x06]; i > ws_ioRam[0x05]; i--) {
            uint32 spr = *ws_sprRamBase--;

            if (!(spr & 0x2000)) {
                // sprite window on ?
                if ((ws_ioRam[0x00] & 0x08) && (spr & 0x1000) && (ws_sprWindow_x0 != ws_sprWindow_x1)) {
                    ws_drawClippedSpriteLine(framebuffer, ws_gpu_scanline, (spr & 0xff000000) >> 24,
                                             (spr & 0x00ff0000) >> 16,
                                             spr & 0x1ff, 8 + ((spr & 0xe00) >> 9), spr & 0x4000, spr & 0x8000,
                                             ws_sprWindow_x0, ws_sprWindow_y0, ws_sprWindow_x1, ws_sprWindow_y1);
                } else {
                    ws_drawClippedSpriteLine(framebuffer, ws_gpu_scanline, (spr & 0xff000000) >> 24,
                                             (spr & 0x00ff0000) >> 16,
                                             spr & 0x1ff, 8 + ((spr & 0xe00) >> 9), spr & 0x4000, spr & 0x8000,
                                             0, 0, 224, 144);
                }
            }
        }
    }
#ifdef STATISTICS
    ws_priority_0_sprites_rendering_time=ticker();
#endif
    // render foreground layer
    if (ws_ioRam[0x00] & 0x02) {
        int ws_fgWindow_x0 = ws_ioRam[0x08];
        int ws_fgWindow_y0 = ws_ioRam[0x09];
        int ws_fgWindow_x1 = ws_ioRam[0x0a];
        int ws_fgWindow_y1 = ws_ioRam[0x0b];
        int ws_fgScroll_x = ws_ioRam[0x12];
        int ws_fgScroll_y = ws_ioRam[0x13];

        int windowMode = ws_ioRam[0x00] & 0x30;

        // seek to the first tile
        ws_fgScroll_y = (ws_fgScroll_y + ws_gpu_scanline) & 0xff;

        // note: byte ordering assumptions!
        int ws_currentTile = (ws_fgScroll_x >> 3);
        uint16 *ws_fgScrollRamBase = (uint16 *) (internalRam + (((uint32) ws_ioRam[0x07] & 0xf0) << 7) +
                                                 ((ws_fgScroll_y & 0xfff8) << 3));

        int lineInTile = ws_fgScroll_y & 0x07;
        int columnInTile = ws_fgScroll_x & 0x07;

        uint8 *scanlinePtr = framebuffer;


        // window disabled
        if (!windowMode) {
            if (ws_gpu_colorActive) {
                // render the first clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

                    ws_tileRow += columnInTile;


                    for (int i = columnInTile; i < 8; i++) {
                            if (*ws_tileRow) *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }

                // render the tiles between them
                int nbTiles = 28;
                if (columnInTile)
                    nbTiles = 27;

                for (int i = 0; i < nbTiles; i++) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

#pragma GCC unroll 8
                    for (int ii = 0; ii < 8; ii++) {
                            if (*ws_tileRow) *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }

                // render the last clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);
//					int16	*wsc_paletteAlias=&wsc_palette[((tileInfo>>9)&0x0f)<<4];


                    for (int i = 0; i < columnInTile; i++) {
                            if (*ws_tileRow) *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }
            } else {
                // render the first clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];
                    ws_tileRow += columnInTile;
                    if ((tileInfo >> 9) & 0x04) {
                        for (int i = columnInTile; i < 8; i++) {
                            if (*ws_tileRow)
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    } else {
                        for (int i = columnInTile; i < 8; i++) {
                            *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    }
                }
                int nbTiles = 28;
                if (columnInTile)
                    nbTiles = 27;

                for (int i = 0; i < nbTiles; i++) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);
                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];

                    if ((tileInfo >> 9) & 0x04) {
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        if (*ws_tileRow) *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                    } else {
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                        *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }

                // render the last clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);
                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];


                    if ((tileInfo >> 9) & 0x04) {
                        for (int i = 0; i < columnInTile; i++) {
                            if (*ws_tileRow)
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    } else {
                        for (int i = 0; i < columnInTile; i++) {
                            *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    }
                }
            }
        } else
            // foreground layer displayed only inside the window
        if (windowMode == 0x20) {
            if (ws_gpu_scanline >= ws_fgWindow_y0 && ws_gpu_scanline <= ws_fgWindow_y1) {
            int column = 0;
            if (ws_gpu_colorActive) {
                // render the first clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

                    ws_tileRow += columnInTile;

                    for (int i = columnInTile; i < 8; i++) {
                        if ((*ws_tileRow) && (column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                            *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        column++;
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }

                // render the tiles between them
                int nbTiles = 28;
                if (columnInTile)
                    nbTiles = 27;

                for (int i = 0; i < nbTiles; i++) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);


                    for (int ii = 0; ii < 8; ii++) {
                        if ((*ws_tileRow) && (column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                            *scanlinePtr =
                                    (((tileInfo
                                            >> 9) &
                                      0x0f)
                                            << 4) +
                                    *ws_tileRow;
                        scanlinePtr++;
                        ws_tileRow++;
                        column++;
                    }
                }

                // render the last clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);


                    for (int i = 0; i < columnInTile; i++) {
                        if ((*ws_tileRow) && (column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                            *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        column++;
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }
            } else {
                // render the first clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];
                    ws_tileRow += columnInTile;

                    if ((tileInfo >> 9) & 0x04) {
                        for (int i = columnInTile; i < 8; i++) {
                            if ((*ws_tileRow) && (column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    } else {
                        for (int i = columnInTile; i < 8; i++) {
                            if ((column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    }
                }
                int nbTiles = 28;
                if (columnInTile)
                    nbTiles = 27;

                for (int i = 0; i < nbTiles; i++) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);
                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];

                    if ((tileInfo >> 9) & 0x04) {
#pragma GCC unroll 8
                        for (int ii = 0; ii < 8; ii++) {
                            if ((*ws_tileRow) && (column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                            column++;
                        }
                    } else {
#pragma GCC unroll 8
                        for (int ii = 0; ii < 8; ii++) {
                            if ((column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                            column++;
                        }
                    }
                }

                // render the last clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);
                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];

                    if ((tileInfo >> 9) & 0x04) {
                        for (int i = 0; i < columnInTile; i++) {
                            if ((*ws_tileRow) && (column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    } else {
                        for (int i = 0; i < columnInTile; i++) {
                            if ((column >= ws_fgWindow_x0) && (column <= ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    }
                }
            }
            }
        } else
            // foreground layer displayed only outside the window
        if (windowMode == 0x30) {
            bool line_outside_y = (ws_gpu_scanline < ws_fgWindow_y0 || ws_gpu_scanline > ws_fgWindow_y1);
            int column = 0;
            if (ws_gpu_colorActive) {
                // render the first clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

                    ws_tileRow += columnInTile;

                    for (int i = columnInTile; i < 8; i++) {
                        if ((*ws_tileRow) && (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1)))
                            *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        column++;
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }

                // render the tiles between them
                int nbTiles = 28;
                if (columnInTile)
                    nbTiles = 27;

                for (int i = 0; i < nbTiles; i++) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

#pragma GCC unroll 8
                    for (int ii = 0; ii < 8; ii++) {
                        if ((*ws_tileRow) && (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1)))
                            *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        scanlinePtr++;
                        ws_tileRow++;
                        column++;
                    }
                }

                // render the last clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);


                    for (int i = 0; i < columnInTile; i++) {
                        if ((*ws_tileRow) && (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1)))
                            *scanlinePtr = (((tileInfo >> 9) & 0x0f) << 4) + *ws_tileRow;
                        column++;
                        scanlinePtr++;
                        ws_tileRow++;
                    }
                }
            } else {
                // render the first clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);

                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];
                    ws_tileRow += columnInTile;

                    if ((tileInfo >> 9) & 0x04) {
                        for (int i = columnInTile; i < 8; i++) {
                            if ((*ws_tileRow) && (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1)))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    } else {
                        for (int i = columnInTile; i < 8; i++) {
                            if (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    }
                }
                int nbTiles = 28;
                if (columnInTile)
                    nbTiles = 27;

                for (int i = 0; i < nbTiles; i++) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile, tileInfo & 0x8000,
                                                                tileInfo & 0x4000, tileInfo & 0x2000);
                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];

                    if ((tileInfo >> 9) & 0x04) {
#pragma GCC unroll 8
                        for (int ii = 0; ii < 8; ii++) {
                            if ((*ws_tileRow) && (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1)))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                            column++;
                        }
                    } else {
#pragma GCC unroll 8
                        for (int ii = 0; ii < 8; ii++) {
                            if (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            scanlinePtr++;
                            ws_tileRow++;
                            column++;
                        }
                    }
                }

                // render the last clipped tile
                if (columnInTile) {
                    uint16 tileInfo = ws_fgScrollRamBase[ws_currentTile & 0x1f];
                    ws_currentTile++;
                    uint8 *ws_tileRow = ws_tileCache_getTileRow(tileInfo & 0x1ff, lineInTile,
                                                                tileInfo & 0x8000, tileInfo & 0x4000,
                                                                tileInfo & 0x2000);
                    int16 *ws_paletteAlias = &ws_palette[((tileInfo >> 9) & 0x0f) << 2];

                    if ((tileInfo >> 9) & 0x04) {
                        for (int i = 0; i < columnInTile; i++) {
                            if ((*ws_tileRow) && (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1)))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    } else {
                        for (int i = 0; i < columnInTile; i++) {
                            if (line_outside_y || (column < ws_fgWindow_x0) || (column > ws_fgWindow_x1))
                                *scanlinePtr = ws_paletteColors[ws_paletteAlias[*ws_tileRow]];
                            column++;
                            scanlinePtr++;
                            ws_tileRow++;
                        }
                    }
                }
            }
        } else {
            // unknown
        }
    }
#ifdef STATISTICS
    ws_foreground_rendering_time=ticker();
#endif
    // render sprites
    if (ws_ioRam[0x00] & 0x04) {
        int ws_sprWindow_x0 = ws_ioRam[0x0c];
        int ws_sprWindow_y0 = ws_ioRam[0x0d];
        int ws_sprWindow_x1 = ws_ioRam[0x0e];
        int ws_sprWindow_y1 = ws_ioRam[0x0f];
        uint32 *ws_sprRamBase = (uint32 *) (internalRam + (((uint32) ws_ioRam[0x04]) << 9));

        // seek to first sprite
        ws_sprRamBase += ws_ioRam[0x06] - 1;

        for (int i = ws_ioRam[0x06]; i > ws_ioRam[0x05]; i--) {
            uint32 spr = *ws_sprRamBase--;

            if (spr & 0x2000) {
                // sprite window on ?
                if ((ws_ioRam[0x00] & 0x08) && (spr & 0x1000) && (ws_sprWindow_x0 != ws_sprWindow_x1)) {
                    ws_drawClippedSpriteLine(framebuffer, ws_gpu_scanline, (spr & 0xff000000) >> 24,
                                             (spr & 0x00ff0000) >> 16,
                                             spr & 0x1ff, 8 + ((spr & 0xe00) >> 9), spr & 0x4000, spr & 0x8000,
                                             ws_sprWindow_x0, ws_sprWindow_y0, ws_sprWindow_x1, ws_sprWindow_y1);
                } else {
                    ws_drawClippedSpriteLine(framebuffer, ws_gpu_scanline, (spr & 0xff000000) >> 24,
                                             (spr & 0x00ff0000) >> 16,
                                             spr & 0x1ff, 8 + ((spr & 0xe00) >> 9), spr & 0x4000, spr & 0x8000,
                                             0, 0, 224, 144);
                }
            }
        }
    }
#ifdef STATISTICS
                                                                                                                            ws_priority_1_sprites_rendering_time=ticker();

	ws_priority_1_sprites_rendering_time-=ws_foreground_rendering_time;
	ws_foreground_rendering_time-=ws_priority_0_sprites_rendering_time;
	ws_priority_0_sprites_rendering_time-=ws_background_rendering_time;
	ws_background_rendering_time-=ws_background_color_rendering_time;
	ws_background_color_rendering_time-=startTime;
#endif
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void __not_in_flash_func(ws_gpu_write_byte)(uint32_t offset, uint8_t value) {
    // ws 4 color tiles
    if ((offset >= 0x2000) && (offset < 0x4000)) {
        if (internalRam[offset] != value)
            ws_modified_tile[(offset & 0x1fff) >> 4] = 1;
        // update the ram
        internalRam[offset] = value;
    }
    if (ws_gpu_operatingInColor) {
        // wsc 16 color tiles bank 0
        if ((offset >= 0x4000) && (offset < 0x8000)) {
            if (internalRam[offset] != value)
                wsc_modified_tile[(offset & 0x3fff) >> 5] = 1;
        } else
            // wsc 16 color tiles bank 1
        if ((offset >= 0x8000) && (offset < 0xC000)) {
            if (internalRam[offset] != value)
                wsc_modified_tile[512 + ((offset & 0x3fff) >> 5)] = 1;
        }

        // update the ram
        internalRam[offset] = value;
        // palette ram
        if (offset >= 0xfe00) {
            // RGB444 format
            uint16 color = (internalRam[(offset & 0xfffe) + 1]);
            color <<= 8;
            color |= (internalRam[(offset & 0xfffe)]);

            int idx = (offset & 0x1ff) >> 1;
            wsc_palette[idx] = (int16)color;

            uint8 r = (color >> 8) & 0xf;
            uint8 g = (color >> 4) & 0xf;
            uint8 b = color & 0xf;
            uint16 r5 = (r << 1) | (r >> 3);
            uint16 g6 = (g << 2) | (g >> 2);
            uint16 b5 = (b << 1) | (b >> 3);
            uint16 c565 = (uint16)((r5 << 11) | (g6 << 5) | b5);
            const uint16 old565 = wsc_palette_rgb565_native[idx];
            wsc_palette_rgb565_native[idx] = c565;
            wsc_palette_rgb565[idx] = __builtin_bswap16(c565);

            /* Tell the display driver. Its dirty-row hash covers the tile
             * indices only, so recolouring a palette entry changes the pixels
             * of every row that uses it while leaving every hash untouched --
             * those rows were being skipped and the old colours stayed on
             * screen. Digimon Tamers showed this as white gaps in the "press
             * start" blink and in the 1P/2P/options menu.
             *
             * Only a colour that really changed counts, and only for its own
             * 16-entry bank: a game that re-uploads the same palette every frame
             * (or fades one bank) must not invalidate every row on screen. The
             * write happens one byte at a time, so the half-written colour of
             * the first byte shows up here as a change too, which is correct --
             * the pixels do change when the second byte lands. */
            if (c565 != old565)
                ili9341_ws_palette_changed((uint32)idx >> 4);
        }
    } else if (offset < 0x4000)
        internalRam[offset] = value;
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////

void ws_gpu_port_write(uint32_t port, uint8_t value) {
    switch (port) {
        case 0x60:
            ws_gpu_changeVideoMode(value);
            return;
        case 0x1C:
            ws_paletteColors[0] = value & 0xf;
            ws_paletteColors[1] = (value >> 4) & 0xf;
            return;
        case 0x1D:
            ws_paletteColors[2] = value & 0xf;
            ws_paletteColors[3] = (value >> 4) & 0xf;
            return;
        case 0x1E:
            ws_paletteColors[4] = value & 0xf;
            ws_paletteColors[5] = (value >> 4) & 0xf;
            return;
        case 0x1F:
            ws_paletteColors[6] = value & 0xf;
            ws_paletteColors[7] = (value >> 4) & 0xf;
            return;
    }
    if ((port >= 0x20) && (port <= 0x3f)) {
        port -= 0x20;
        int paletteIndex = port >> 1;
        if (port & 0x01) {
            ws_palette[(paletteIndex << 2) + 2] = value & 0x7;
            ws_palette[(paletteIndex << 2) + 3] = (value >> 4) & 0x7;
        } else {
            ws_palette[(paletteIndex << 2) + 0] = value & 0x7;
            ws_palette[(paletteIndex << 2) + 1] = (value >> 4) & 0x7;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint8_t ws_gpu_port_read(uint8_t port) {
    if (port == 0xa0) {
        if (ws_gpu_forceColorSystemBool)
            return ws_ioRam[0xa0] | 2;
        else if (ws_gpu_forceMonoSystemBool)
            return ws_ioRam[0xa0] & (~0x02);
        else {
            if (ws_gpu_operatingInColor)
                return ws_ioRam[0xa0] | 2;
            else
                return ws_ioRam[0xa0] & (~0x02);
        }
    }
    return (ws_ioRam[port]);
}
