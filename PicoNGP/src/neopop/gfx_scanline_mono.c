//---------------------------------------------------------------------------
// NEOPOP : Emulator as in Dreamland
//
// Copyright (c) 2001-2002 by neopop_uk
//---------------------------------------------------------------------------

#pragma GCC optimize("Ofast")
#include "pico.h"
#include "neopop.h"
#include "mem.h"
#include "gfx.h"
#include <stdint.h>

//=============================================================================

// Precalculated 12-bit 0BGR palettes for mono intensities 0..7
// Intensity formula: val = (c << 1) | (c << 5) | (c << 9)
static const _u16 s_mono_lut_pos[8] = {
	(_u16)~0x0000,
	(_u16)~0x0222,
	(_u16)~0x0444,
	(_u16)~0x0666,
	(_u16)~0x0888,
	(_u16)~0x0AAA,
	(_u16)~0x0CCC,
	(_u16)~0x0EEE
};

static const _u16 s_mono_lut_neg[8] = {
	0x0000,
	0x0222,
	0x0444,
	0x0666,
	0x0888,
	0x0AAA,
	0x0CCC,
	0x0EEE
};

static inline __attribute__((always_inline)) void Plot(_u8 x, const _u8* pal_base, _u8 index, const _u16* mono_lut, _u8 depth)
{
	//Clip
	if (__builtin_expect(x < winx || x >= (winw + winx) || x >= SCREEN_WIDTH, 0))
		return;

	//Depth check, <= to stop later sprites overwriting pixels!
	if (depth <= zbuffer[x]) return;
	zbuffer[x] = depth;

	cfb_scanline[x] = mono_lut[pal_base[index] & 7];
}

static inline __attribute__((always_inline)) void Plot_fast(_u8 x, const _u8* pal_base, _u8 index, const _u16* mono_lut, _u8 depth)
{
	if (depth <= zbuffer[x]) return;
	zbuffer[x] = depth;

	cfb_scanline[x] = mono_lut[pal_base[index] & 7];
}

static inline __attribute__((always_inline)) void drawPattern_with_data(_u8 screenx, _u16 data, _u16 mirror,
				 const _u8* pal_base, const _u16* mono_lut, _u8 depth)
{
	//Quick rejection: tile is completely off-screen to the right
	if (screenx >= SCREEN_WIDTH && screenx <= 248)
		return;

	_u8 max_vis = min1((_u8)(winw + winx), SCREEN_WIDTH);

	//Fast path: completely inside screen and window bounds
	if (__builtin_expect(screenx >= winx && (screenx + 8) <= max_vis, 1))
	{
		if (mirror)
		{
			_u8 p;
			p = (_u8)((data >> 14) & 3); if (p) Plot_fast(screenx + 7, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 12) & 3); if (p) Plot_fast(screenx + 6, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 10) & 3); if (p) Plot_fast(screenx + 5, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 8) & 3);  if (p) Plot_fast(screenx + 4, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 6) & 3);  if (p) Plot_fast(screenx + 3, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 4) & 3);  if (p) Plot_fast(screenx + 2, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 2) & 3);  if (p) Plot_fast(screenx + 1, pal_base, p, mono_lut, depth);
			p = (_u8)(data & 3);         if (p) Plot_fast(screenx + 0, pal_base, p, mono_lut, depth);
		}
		else
		{
			_u8 p;
			p = (_u8)((data >> 14) & 3); if (p) Plot_fast(screenx + 0, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 12) & 3); if (p) Plot_fast(screenx + 1, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 10) & 3); if (p) Plot_fast(screenx + 2, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 8) & 3);  if (p) Plot_fast(screenx + 3, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 6) & 3);  if (p) Plot_fast(screenx + 4, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 4) & 3);  if (p) Plot_fast(screenx + 5, pal_base, p, mono_lut, depth);
			p = (_u8)((data >> 2) & 3);  if (p) Plot_fast(screenx + 6, pal_base, p, mono_lut, depth);
			p = (_u8)(data & 3);         if (p) Plot_fast(screenx + 7, pal_base, p, mono_lut, depth);
		}
		return;
	}

	//Boundary path (clipping needed)
	if (mirror)
	{
		_u8 p;
		p = (_u8)((data >> 14) & 3); if (p) Plot(screenx + 7, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 12) & 3); if (p) Plot(screenx + 6, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 10) & 3); if (p) Plot(screenx + 5, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 8) & 3);  if (p) Plot(screenx + 4, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 6) & 3);  if (p) Plot(screenx + 3, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 4) & 3);  if (p) Plot(screenx + 2, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 2) & 3);  if (p) Plot(screenx + 1, pal_base, p, mono_lut, depth);
		p = (_u8)(data & 3);         if (p) Plot(screenx + 0, pal_base, p, mono_lut, depth);
	}
	else
	{
		_u8 p;
		p = (_u8)((data >> 14) & 3); if (p) Plot(screenx + 0, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 12) & 3); if (p) Plot(screenx + 1, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 10) & 3); if (p) Plot(screenx + 2, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 8) & 3);  if (p) Plot(screenx + 3, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 6) & 3);  if (p) Plot(screenx + 4, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 4) & 3);  if (p) Plot(screenx + 5, pal_base, p, mono_lut, depth);
		p = (_u8)((data >> 2) & 3);  if (p) Plot(screenx + 6, pal_base, p, mono_lut, depth);
		p = (_u8)(data & 3);         if (p) Plot(screenx + 7, pal_base, p, mono_lut, depth);
	}
}

static void gfx_draw_scroll1(const _u16* mono_lut, _u8 depth)
{
	_u8 i, row, line;
	_u16 data16;

	line = scanline + scroll1y;
	row = line & 7;
	_u32 row_offset = ((line >> 3) & 0x1F) << 5;

	_u8 start_tx = (scroll1x >> 3) & 0x1F;
	for (i = 0; i < 21; i++)
	{
		_u8 tx = (start_tx + i) & 0x1F;
		data16 = *(_u16*)(ram + 0x9000 + ((tx + row_offset) << 1));
		_u16 tile = data16 & 0x01FF;
		if (!tile) continue; // Tile 0 is transparent!

		_u8 tiley = (data16 & 0x4000) ? (7 - row) : row;
		_u16 data = *(_u16*)(ram + 0xA000 + (tile * 16) + (tiley * 2));
		if (!data) continue; // Tile row is transparent, quick bypass!

		const _u8* pal_base = (data16 & 0x2000) ? (ram + 0x810C) : (ram + 0x8108);
		drawPattern_with_data(
			(_u8)((tx << 3) - scroll1x),
			data,
			data16 & 0x8000,
			pal_base,
			mono_lut,
			depth);
	}
}

static void gfx_draw_scroll2(const _u16* mono_lut, _u8 depth)
{
	_u8 i, row, line;
	_u16 data16;

	line = scanline + scroll2y;
	row = line & 7;
	_u32 row_offset = ((line >> 3) & 0x1F) << 5;

	_u8 start_tx = (scroll2x >> 3) & 0x1F;
	for (i = 0; i < 21; i++)
	{
		_u8 tx = (start_tx + i) & 0x1F;
		data16 = *(_u16*)(ram + 0x9800 + ((tx + row_offset) << 1));
		_u16 tile = data16 & 0x01FF;
		if (!tile) continue; // Tile 0 is transparent!

		_u8 tiley = (data16 & 0x4000) ? (7 - row) : row;
		_u16 data = *(_u16*)(ram + 0xA000 + (tile * 16) + (tiley * 2));
		if (!data) continue; // Tile row is transparent, quick bypass!

		const _u8* pal_base = (data16 & 0x2000) ? (ram + 0x8114) : (ram + 0x8110);
		drawPattern_with_data(
			(_u8)((tx << 3) - scroll2x),
			data,
			data16 & 0x8000,
			pal_base,
			mono_lut,
			depth);
	}
}

/* Cached Active Sprite List for Mono */
typedef struct {
    _s16 x;
    _s16 y;
    _u16 data16;
    _u8 pal;
    _u8 priority_x2;
} CachedSpriteMono;

static CachedSpriteMono s_active_sprites_mono[64];
static int s_active_sprite_count_mono = 0;
bool s_sprites_cached_this_frame_mono = false;

static void gfx_cache_sprites_mono(void)
{
    _s16 lastSpriteX = 0;
    _s16 lastSpriteY = 0;
    s_active_sprite_count_mono = 0;

    for (int spr = 0; spr < 64; spr++)
    {
        _u32 spr_raw = *(_u32*)(ram + 0x8800 + (spr << 2));
        _u16 data16 = (_u16)(spr_raw & 0xFFFF);
        _u8 sx = (_u8)(spr_raw >> 16);
        _u8 sy = (_u8)(spr_raw >> 24);
        _s16 x = sx;
        _s16 y = sy;

        _u8 priority = (data16 & 0x1800) >> 11;

        if (data16 & 0x0400) x = lastSpriteX + sx;
        if (data16 & 0x0200) y = lastSpriteY + sy;

        lastSpriteX = x;
        lastSpriteY = y;

        if (priority == 0) continue;

        x += scrollsprx;
        y += scrollspry;

        if (x > 248 && x < 256) x = x - 256; else x &= 0xFF;
        if (y > 248 && y < 256) y = y - 256; else y &= 0xFF;

        if (y >= SCREEN_HEIGHT && y < 248) continue;
        if (x >= SCREEN_WIDTH && x < 248) continue;

        CachedSpriteMono *cs = &s_active_sprites_mono[s_active_sprite_count_mono++];
        cs->x = x;
        cs->y = y;
        cs->data16 = data16;
        cs->pal = (data16 & 0x2000) ? 1 : 0;
        cs->priority_x2 = priority << 1;
    }
}

void __not_in_flash_func(gfx_draw_scanline_mono)(void)
{
	scanline = ram[0x8009];
	cfb_scanline = cfb;

	if (!s_sprites_cached_this_frame_mono)
	{
		gfx_cache_sprites_mono();
		s_sprites_cached_this_frame_mono = true;
	}

	const _u16* mono_lut = negative ? s_mono_lut_neg : s_mono_lut_pos;

	// Fast 32-bit clear of zbuffer (40 uint32 words = 160 bytes)
	uint32_t *z32 = (uint32_t *)zbuffer;
	for (int i = 0; i < 40; i++) z32[i] = 0;

	// Window colour (out of window color)
	_u16 oow_c = mono_lut[oowc & 7];

	// Background colour
	_u16 bg_c;
	if (scanline >= winy && scanline < winy + winh)
	{
		if ((bgc & 0xC0) == 0x80)
			bg_c = mono_lut[bgc & 7];
		else
			bg_c = negative ? 0x0000 : 0x0FFF;
	}
	else
	{
		bg_c = oow_c;
	}

	// Fast 32-bit background fill (80 uint32 words = 160 pixels)
	uint32_t bg32 = (uint32_t)bg_c | ((uint32_t)bg_c << 16);
	uint32_t *cfb32 = (uint32_t *)cfb_scanline;
	for (int i = 0; i < 80; i++) cfb32[i] = bg32;

	// Non-standard window margins (rare)
	if (__builtin_expect(winx > 0 || (winx + winw) < SCREEN_WIDTH, 0))
	{
		for (int x = 0; x < min1(winx, SCREEN_WIDTH); x++) cfb_scanline[x] = oow_c;
		for (int x = min1((_u8)(winx + winw), SCREEN_WIDTH); x < SCREEN_WIDTH; x++) cfb_scanline[x] = oow_c;
	}

	// Draw inside the active window
	if (scanline >= winy && scanline < winy + winh)
	{
		//Swap Front/Back scroll planes?
		if (planeSwap)
		{
			gfx_draw_scroll1(mono_lut, ZDEPTH_BACKGROUND_SCROLL);
			gfx_draw_scroll2(mono_lut, ZDEPTH_FOREGROUND_SCROLL);
		}
		else
		{
			gfx_draw_scroll2(mono_lut, ZDEPTH_BACKGROUND_SCROLL);
			gfx_draw_scroll1(mono_lut, ZDEPTH_FOREGROUND_SCROLL);
		}

		// Draw Active Sprites
		for (int i = 0; i < s_active_sprite_count_mono; i++)
		{
			_s16 y = s_active_sprites_mono[i].y;
			if (scanline >= y && scanline <= y + 7)
			{
				_u8 row = (scanline - y) & 7;
				_u16 sdata = s_active_sprites_mono[i].data16;
				_u16 tile = sdata & 0x01FF;
				_u8 tiley = (sdata & 0x4000) ? (7 - row) : row;
				_u16 data = *(_u16*)(ram + 0xA000 + (tile * 16) + (tiley * 2));
				if (!data) continue;

				const _u8* pal_base = s_active_sprites_mono[i].pal ? (ram + 0x8104) : (ram + 0x8100);
				drawPattern_with_data(
					(_u8)s_active_sprites_mono[i].x,
					data,
					sdata & 0x8000,
					pal_base,
					mono_lut,
					s_active_sprites_mono[i].priority_x2);
			}
		}
	}
}
