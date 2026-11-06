//---------------------------------------------------------------------------
// NEOPOP : Emulator as in Dreamland
//
// Copyright (c) 2001-2002 by neopop_uk
//---------------------------------------------------------------------------

#include "neopop.h"
#include "mem.h"
#include "gfx.h"
#include <stdint.h>
#include "pico.h"

//=============================================================================

static inline __attribute__((always_inline)) void Plot(_u8 x, const _u16* pal_base, _u8 index, _u8 depth)
{
	//Clip
	if (x < winx || x >= (winw + winx) || x >= SCREEN_WIDTH)
		return;

	//Depth check, <= to stop later sprites overwriting pixels!
	if (depth <= zbuffer[x]) return;
	zbuffer[x] = depth;

	_u16 data16 = pal_base[index];

	if (__builtin_expect(negative != 0, 0))
		cfb_scanline[x] = ~data16;
	else
		cfb_scanline[x] = data16;
}

static inline __attribute__((always_inline)) void Plot_fast(_u8 x, const _u16* pal_base, _u8 index, _u8 depth)
{
	if (depth <= zbuffer[x]) return;
	zbuffer[x] = depth;

	_u16 data16 = pal_base[index];

	if (__builtin_expect(negative != 0, 0))
		cfb_scanline[x] = ~data16;
	else
		cfb_scanline[x] = data16;
}

static inline __attribute__((always_inline)) void drawPattern_with_data(_u8 screenx, _u16 data, _u16 mirror, 
				 const _u16* pal_base, _u8 depth)
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
			p = (_u8)((data >> 14) & 3); if (p) Plot_fast(screenx + 7, pal_base, p, depth);
			p = (_u8)((data >> 12) & 3); if (p) Plot_fast(screenx + 6, pal_base, p, depth);
			p = (_u8)((data >> 10) & 3); if (p) Plot_fast(screenx + 5, pal_base, p, depth);
			p = (_u8)((data >> 8) & 3);  if (p) Plot_fast(screenx + 4, pal_base, p, depth);
			p = (_u8)((data >> 6) & 3);  if (p) Plot_fast(screenx + 3, pal_base, p, depth);
			p = (_u8)((data >> 4) & 3);  if (p) Plot_fast(screenx + 2, pal_base, p, depth);
			p = (_u8)((data >> 2) & 3);  if (p) Plot_fast(screenx + 1, pal_base, p, depth);
			p = (_u8)(data & 3);         if (p) Plot_fast(screenx + 0, pal_base, p, depth);
		}
		else
		{
			_u8 p;
			p = (_u8)((data >> 14) & 3); if (p) Plot_fast(screenx + 0, pal_base, p, depth);
			p = (_u8)((data >> 12) & 3); if (p) Plot_fast(screenx + 1, pal_base, p, depth);
			p = (_u8)((data >> 10) & 3); if (p) Plot_fast(screenx + 2, pal_base, p, depth);
			p = (_u8)((data >> 8) & 3);  if (p) Plot_fast(screenx + 3, pal_base, p, depth);
			p = (_u8)((data >> 6) & 3);  if (p) Plot_fast(screenx + 4, pal_base, p, depth);
			p = (_u8)((data >> 4) & 3);  if (p) Plot_fast(screenx + 5, pal_base, p, depth);
			p = (_u8)((data >> 2) & 3);  if (p) Plot_fast(screenx + 6, pal_base, p, depth);
			p = (_u8)(data & 3);         if (p) Plot_fast(screenx + 7, pal_base, p, depth);
		}
		return;
	}

	//Boundary path (clipping needed)
	if (mirror)
	{
		_u8 p;
		p = (_u8)((data >> 14) & 3); if (p) Plot(screenx + 7, pal_base, p, depth);
		p = (_u8)((data >> 12) & 3); if (p) Plot(screenx + 6, pal_base, p, depth);
		p = (_u8)((data >> 10) & 3); if (p) Plot(screenx + 5, pal_base, p, depth);
		p = (_u8)((data >> 8) & 3);  if (p) Plot(screenx + 4, pal_base, p, depth);
		p = (_u8)((data >> 6) & 3);  if (p) Plot(screenx + 3, pal_base, p, depth);
		p = (_u8)((data >> 4) & 3);  if (p) Plot(screenx + 2, pal_base, p, depth);
		p = (_u8)((data >> 2) & 3);  if (p) Plot(screenx + 1, pal_base, p, depth);
		p = (_u8)(data & 3);         if (p) Plot(screenx + 0, pal_base, p, depth);
	}
	else
	{
		_u8 p;
		p = (_u8)((data >> 14) & 3); if (p) Plot(screenx + 0, pal_base, p, depth);
		p = (_u8)((data >> 12) & 3); if (p) Plot(screenx + 1, pal_base, p, depth);
		p = (_u8)((data >> 10) & 3); if (p) Plot(screenx + 2, pal_base, p, depth);
		p = (_u8)((data >> 8) & 3);  if (p) Plot(screenx + 3, pal_base, p, depth);
		p = (_u8)((data >> 6) & 3);  if (p) Plot(screenx + 4, pal_base, p, depth);
		p = (_u8)((data >> 4) & 3);  if (p) Plot(screenx + 5, pal_base, p, depth);
		p = (_u8)((data >> 2) & 3);  if (p) Plot(screenx + 6, pal_base, p, depth);
		p = (_u8)(data & 3);         if (p) Plot(screenx + 7, pal_base, p, depth);
	}
}

static void gfx_draw_scroll1(_u8 depth)
{
	_u8 i, row, line;
	_u16 data16;

	line = scanline + scroll1y;
	row = line & 7;	//Which row?
	_u32 row_offset = ((line >> 3) & 0x1F) << 5;

	//Draw Foreground scroll plane (Scroll 1) - 21 tiles visíveis cobrem 160 px com scroll
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

		const _u16* pal_base = (_u16*)(ram + 0x8280) + (((data16 & 0x1E00) >> 9) << 2);
		drawPattern_with_data(
                (_u8)((tx << 3) - scroll1x),
                data,
                data16 & 0x8000,
                pal_base,
                depth);
	}
}

static void gfx_draw_scroll2(_u8 depth)
{
	_u8 i, row, line;
	_u16 data16;

	line = scanline + scroll2y;
	row = line & 7;	//Which row?
	_u32 row_offset = ((line >> 3) & 0x1F) << 5;

	//Draw Background scroll plane (Scroll 2) - 21 tiles visíveis cobrem 160 px com scroll
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

		const _u16* pal_base = (_u16*)(ram + 0x8300) + (((data16 & 0x1E00) >> 9) << 2);
		drawPattern_with_data(
                (_u8)((tx << 3) - scroll2x),
                data,
                data16 & 0x8000,
                pal_base,
                depth);
	}
}

/* Cached Active Sprite List (evaluated once at scanline 0 per frame) */
typedef struct {
    _s16 x;
    _s16 y;
    _u16 data16;
    _u8 pal;
    _u8 priority_x2;
} CachedSprite;

static CachedSprite s_active_sprites[64];
static int s_active_sprite_count = 0;
bool s_sprites_cached_this_frame = false;

static void gfx_cache_sprites(void)
{
    _s16 lastSpriteX = 0;
    _s16 lastSpriteY = 0;
    s_active_sprite_count = 0;

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

        // Ignore sprites completely outside active display bounds
        if (y >= SCREEN_HEIGHT && y < 248) continue;
        if (x >= SCREEN_WIDTH && x < 248) continue;

        s_active_sprites[s_active_sprite_count].x = x;
        s_active_sprites[s_active_sprite_count].y = y;
        s_active_sprites[s_active_sprite_count].data16 = data16;
        s_active_sprites[s_active_sprite_count].pal = ram[0x8C00 + spr] & 0xF;
        s_active_sprites[s_active_sprite_count].priority_x2 = priority << 1;
        s_active_sprite_count++;
    }
}

void __not_in_flash_func(gfx_draw_scanline_colour)(void)
{
	int x;
	_u16 data16;

	//Get the current scanline
	scanline = ram[0x8009];
	cfb_scanline = cfb;

	if (!s_sprites_cached_this_frame)
	{
		gfx_cache_sprites();
		s_sprites_cached_this_frame = true;
	}

	// Fast 32-bit clear of zbuffer (40 uint32 words = 160 bytes)
	uint32_t *z32 = (uint32_t *)zbuffer;
	for (int i = 0; i < 40; i++) z32[i] = 0;

	// Determine background color
	data16 = *(_u16*)(ram + 0x83F0 + (oowc << 1));
	if (negative) data16 = ~data16;

	if (scanline >= winy && scanline < winy + winh)
	{
		data16 = *(_u16*)(_u8*)(ram + 0x83E0 + ((bgc & 7) << 1));
		if (negative) data16 = ~data16;
	}

	// Fast 32-bit background fill (80 uint32 words = 160 pixels)
	uint32_t bg32 = (uint32_t)data16 | ((uint32_t)data16 << 16);
	uint32_t *cfb32 = (uint32_t *)cfb_scanline;
	for (int i = 0; i < 80; i++) cfb32[i] = bg32;

	// Non-standard window margins (rare)
	if (__builtin_expect(winx > 0 || (winx + winw) < SCREEN_WIDTH, 0))
	{
		_u16 oow_c = *(_u16*)(ram + 0x83F0 + (oowc << 1));
		if (negative) oow_c = ~oow_c;
		for (x = 0; x < min1(winx, SCREEN_WIDTH); x++) cfb_scanline[x] = oow_c;
		for (x = min1((_u8)(winx + winw), SCREEN_WIDTH); x < SCREEN_WIDTH; x++) cfb_scanline[x] = oow_c;
	}

	// Draw inside the active window
	if (scanline >= winy && scanline < winy + winh)
	{
		//Swap Front/Back scroll planes?
		if (planeSwap)
		{
			gfx_draw_scroll1(ZDEPTH_BACKGROUND_SCROLL);		//Swap
			gfx_draw_scroll2(ZDEPTH_FOREGROUND_SCROLL);
		}
		else
		{
			gfx_draw_scroll2(ZDEPTH_BACKGROUND_SCROLL);		//Normal
			gfx_draw_scroll1(ZDEPTH_FOREGROUND_SCROLL);
		}

		//Draw Active Sprites (tested only over pre-filtered visible sprites)
		for (int i = 0; i < s_active_sprite_count; i++)
		{
			_s16 y = s_active_sprites[i].y;
			if (scanline >= y && scanline <= y + 7)
			{
				_u8 row = (scanline - y) & 7;
				_u16 sdata = s_active_sprites[i].data16;
				_u16 tile = sdata & 0x01FF;
				_u8 tiley = (sdata & 0x4000) ? (7 - row) : row;
				_u16 data = *(_u16*)(ram + 0xA000 + (tile * 16) + (tiley * 2));
				if (!data) continue;

				const _u16* pal_base = (_u16*)(ram + 0x8200) + (s_active_sprites[i].pal << 2);
				drawPattern_with_data((_u8)s_active_sprites[i].x, data,
					sdata & 0x8000,
					pal_base,
					s_active_sprites[i].priority_x2);
			}
		}
	}
}

//=============================================================================
