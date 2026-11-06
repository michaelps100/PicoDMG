/* PicoSNES port glue: the host-side symbols snes9x's core expects the port to
 * provide, derived from PicoPlus-devel/pico-snesPlus's port_glue.cpp.
 *
 * What changed against upstream, and why:
 *
 *   - The copy-out no longer writes an HSTX framebuffer. This console has an
 *     ILI9341 or ST7789 on SPI0 with a Core 1 slot-queue blitter, so the finished
 *     rows are handed to ili9341_stream_snes_line() instead of memcpy'd into a
 *     320x240 window. That is the same seam, a different sink -- and it is the
 *     one path already proven working on this hardware, since the diagnostics
 *     showed core == drv on every frame (what the rasteriser emits arrives at
 *     the driver intact).
 *
 *   - S9xReadJoypad returns the pad mask main.cpp assembled, instead of reading
 *     USB/XInput/Wii controllers. The console has eight GPIO buttons.
 *
 * The strip renderer itself is upstream's, unchanged: S9xUpdateScreen renders a
 * block of scanlines in chunks of S9X_STRIP_ROWS into four small SRAM staging
 * strips, then calls s9x_port_strip_copyout() with the finished rows. Keeping
 * screen/subscreen/both Z buffers in SRAM is what keeps the per-pixel render
 * traffic off the PSRAM bus. */

#include <stdio.h>
#include <string.h>

extern "C" {
#include "snes9x.h"
#include "memmap.h"
#include "gfx.h"
#include "display.h"
#include "ppu.h"
}
#include "port_alloc.h"

#if USE_ILI9341
#include "ili9341.h"
#endif

/* Provided by main.cpp: the physical buttons, already through the shared combo
 * layer and already mapped to SNES bits (SNES B on console A, SNES Y on console
 * B -- see main.cpp). */
extern "C" uint32_t s_current_pad_mask;

#define SNES_STRIP_W  SNES_WIDTH

/* +1 guard row: the force-lores path in gfx.c contains modes 5/6, and their
 * double-width writers still spill up to one row past the last chunk row. */
#define STRIP_GUARD_ROWS (S9X_STRIP_ROWS + 1)

static uint8_t *strip_screen;
static uint8_t *strip_sub;
static uint8_t *strip_z;
static uint8_t *strip_subz;

extern "C" {
/* Last row the PPU may emit, so a mid-frame ScreenHeight flip (224 <-> 239 when
 * the overscan bit changes) can be clamped rather than writing past the window. */
int s9x_port_max_endy = SNES_HEIGHT - 1;

/* 239x128 scratch for S9xSetupOBJ's FirstSprite+Y case -- it used to abuse
 * GFX.SubScreen, which is now a strip far too small for it. PSRAM, same tier
 * SubScreen was in before. */
uint8_t *s9x_port_objonline = nullptr;

/* Counters for the on-card diagnostic log (0:/saves/snes/diag.txt). The console
 * has no UART, so this is the only way to tell "core rendered nothing" apart
 * from "core rendered but the driver dropped it" without a scope. Reset once a
 * second by main.cpp's snes_frame_diagnostics(). */
uint32_t s9x_port_copyout_calls = 0;  /* s9x_port_strip_copyout() invocations */
uint32_t s9x_port_lines_published = 0;/* scanlines handed to the panel */
uint32_t s9x_port_strip_px_nonzero = 0;/* non-zero pixels seen in the strips */

bool s9x_interleave_enabled = false;
uint8_t s9x_line_active[256] = {0};

void s9x_update_interleave_field(void)
{
    if (!s9x_interleave_enabled) {
        memset(s9x_line_active, 1, sizeof(s9x_line_active));
    } else {
        for (int i = 0; i < 256; i++) {
#if USE_ILI9341
            s9x_line_active[i] = ili9341_snes_line_active(i) ? 1 : 0;
#else
            s9x_line_active[i] = 1;
#endif
        }
    }
}

void s9x_set_interleave(bool enable)
{
    s9x_interleave_enabled = enable;
    s9x_update_interleave_field();
}
}

/* Re-anchor after PPU.ScreenHeight changes at runtime (the overscan bit flips
 * it between 224 and 239). There is no framebuffer window to re-anchor here --
 * the driver owns geometry -- so this only tracks the bound. */
extern "C" void s9x_port_anchor_screen(void)
{
    s9x_port_max_endy = (PPU.ScreenHeight ? PPU.ScreenHeight : SNES_HEIGHT) - 1;
}

/* Point the GFX buffers at the strips so absolute rows [row, row+N-1] land at
 * strip rows [0, N-1]. The intermediate (base - row*pitch) points below the
 * allocation; only rows >= row are ever dereferenced.
 *
 * Both per-chunk helpers run from SRAM (__not_in_flash_func): they are called
 * 14+ times per rendered frame from the SRAM-resident gfx.c and must not fetch
 * through the XIP/QMI bus they exist to relieve. */
extern "C" void __not_in_flash_func(s9x_port_strip_repoint)(uint32_t row)
{
    GFX.Screen     = strip_screen - (size_t)row * SNES_STRIP_W * 2;
    GFX.SubScreen  = strip_sub    - (size_t)row * SNES_STRIP_W * 2;
    GFX.ZBuffer    = strip_z      - (size_t)row * SNES_STRIP_W;
    GFX.SubZBuffer = strip_subz   - (size_t)row * SNES_STRIP_W;
}

/* Publish finished rows [start_row, end_row] to the panel.
 *
 * One call per scanline into the ILI9341/ST7789 slot queue, which owns the
 * scaling, the scanline effect, the volume HUD and the Core 1 SPI streaming. The
 * driver is told the row's absolute screen position, so it places the line in
 * the panel itself; nothing here needs to know about scale modes or the GB
 * window.
 *
 * __not_in_flash_func for the same reason as s9x_port_strip_repoint. */
extern "C" void s9x_port_frame_published(int screen_h);

/* Panel row the picture occupies, as a fixed height.
 *
 * The SNES's overscan register ($2133) makes the core flip PPU.ScreenHeight
 * between 224 and 239 *while a game is running* (ppu.c:560-573). Passing that
 * straight through to the driver is what broke GB_MODE here: every flip called
 * ili9341_md_set_geometry(), which recomputed s_md_panel_y, invalidated the
 * xmap and rewrote the whole border region -- mid-frame, so the top and bottom
 * of the picture came from one geometry and the middle from another, and the
 * black side borders moved with it.
 *
 * So the driver is told 240 rows, once, and the picture is placed inside it
 * without the geometry ever changing. s9x_port_screen_top() gives the vertical
 * offset for a given source height so the image stays centred and the leftover
 * rows stay black; both 224 and 239 fit inside 240 without scaling, which is
 * also what makes GB_MODE's 266x240 window land correctly.
 *
 * 240 is the panel height and also DMG_GB_WINDOW_H, so in GB_MODE the picture
 * and the window occupy exactly the same rows and the xmap alone decides the
 * horizontal placement. */
#define SNES_PANEL_H ILI9341_HEIGHT

static inline int s9x_port_screen_top(int source_h)
{
    if (source_h >= SNES_PANEL_H) return 0;
    return (SNES_PANEL_H - source_h) / 2;
}

extern "C" void __not_in_flash_func(s9x_port_strip_copyout)(uint32_t start_row, uint32_t end_row)
{
#if USE_ILI9341
    const uint8_t *src = strip_screen;
    const int source_h = PPU.ScreenHeight ? (int)PPU.ScreenHeight : SNES_HEIGHT;
    s9x_port_copyout_calls++;
#ifndef ENABLE_DIAGNOSTIC_FRAME_METRICS
#define ENABLE_DIAGNOSTIC_FRAME_METRICS 0
#endif

    for (uint32_t y = start_row; y <= end_row; y++) {
        const uint16_t *row = (const uint16_t *)src;
        if (!s9x_interleave_enabled || s9x_line_active[y]) {
#if ENABLE_DIAGNOSTIC_FRAME_METRICS
            for (int x = 0; x < SNES_STRIP_W; x++)
                if (row[x]) { s9x_port_strip_px_nonzero++; break; }
#endif
            /* Source row y and source height. Driver handles vertical placement and scaling. */
            ili9341_stream_snes_line((int)y, row, SNES_STRIP_W, source_h);
            s9x_port_lines_published++;
        }
        src += SNES_STRIP_W * 2;
    }
    if (end_row >= (uint32_t)(source_h - 1))
        s9x_port_frame_published(source_h);
#else
    (void)start_row;
    (void)end_row;
#endif
}

/* End of frame, from the driver's point of view. The core has no per-frame
 * callback in this revision (no S9xDeinitUpdate), so the natural place to close
 * a frame is the copy-out that reaches the last row. Reporting it here keeps the
 * driver's end-of-frame bookkeeping on the same code path that fed it. */
extern "C" void s9x_port_frame_published(int screen_h)
{
#if USE_ILI9341
    ili9341_snes_set_source_height(screen_h);
    ili9341_stream_snes_frame_end();
    s9x_update_interleave_field();
#else
    (void)screen_h;
#endif
}

/* SRAM-first with PSRAM fallback, so a strip falling back to PSRAM is visible
 * in the log rather than silently costing the bandwidth win it was meant to
 * avoid. */
static uint8_t *strip_alloc(size_t bytes, const char *name)
{
    uint8_t *p = (uint8_t *)port_alloc_sram(bytes);
    if (p) {
        printf("%s (%u B) in SRAM\n", name, (unsigned)bytes);
        return p;
    }
    p = (uint8_t *)port_alloc_psram(bytes);
    printf("%s (%u B) in PSRAM (SRAM heap full)\n", name, (unsigned)bytes);
    return p;
}

extern "C" bool S9xInitDisplay(void)
{
    GFX.Pitch  = SNES_STRIP_W * 2;   /* native 512 -- strips decouple us from the panel */
    GFX.ZPitch = SNES_STRIP_W;
    s9x_port_anchor_screen();

    /* The two 16-bit strips come from ONE allocation, and the two 8-bit strips
     * from a second.
     *
     * Not an optimisation -- a correctness requirement I got wrong on the first
     * pass. S9xInitGFX() derives the line strides from the *pointer difference*
     * between the two buffers:
     *
     *     GFX.Delta       = (GFX.SubScreen - GFX.Screen) >> 1;
     *     GFX.DepthDelta  =  GFX.SubZBuffer - GFX.ZBuffer;
     *
     * Upstream gets this right by construction because it allocates screen and
     * subscreen as one block with SubScreen immediately after Screen, so that
     * difference is the row stride. With four independent mallocs it is the
     * distance between two unrelated heap blocks -- on this board it came out
     * 4356 instead of 256, and GFX.Delta is the per-scanline colour-buffer
     * stride. Every pixel store then landed outside its own row, which is why
     * the core published a full 224 lines per frame while every one of them was
     * zero, and vram_nz stayed at 0.
     *
     * The sub-blocks stay in one allocation so that the arithmetic above yields
     * the stride; the four repoint helpers below subtract `row * width` from each
     * base independently, so contiguous bases cost nothing at render time.
     */
    const size_t screen_bytes = (size_t)STRIP_GUARD_ROWS * SNES_STRIP_W * 2;
    const size_t zbytes       = (size_t)STRIP_GUARD_ROWS * SNES_STRIP_W;

    uint8_t *pair16 = (uint8_t *)port_alloc_sram(screen_bytes * 2);
    if (pair16) {
        printf("strip screen+sub (%u B) in SRAM\n", (unsigned)(screen_bytes * 2));
    } else {
        pair16 = (uint8_t *)port_alloc_psram(screen_bytes * 2);
        printf("strip screen+sub (%u B) in PSRAM (SRAM heap full)\n",
               (unsigned)(screen_bytes * 2));
    }
    uint8_t *pair8 = (uint8_t *)port_alloc_sram(zbytes * 2);
    if (pair8) {
        printf("strip z+subz (%u B) in SRAM\n", (unsigned)(zbytes * 2));
    } else {
        pair8 = (uint8_t *)port_alloc_psram(zbytes * 2);
        printf("strip z+subz (%u B) in PSRAM (SRAM heap full)\n", (unsigned)(zbytes * 2));
    }

    if (!pair16 || !pair8) return false;

    strip_screen = pair16;
    strip_sub    = pair16 + screen_bytes;   /* immediately after: keeps GFX.Delta == 256 */
    strip_z      = pair8;
    strip_subz   = pair8 + zbytes;          /* keeps GFX.DepthDelta == 256 */

    /* Sprite visibility table (32 KB): PSRAM */
    s9x_port_objonline = (uint8_t *)port_alloc_psram((size_t)SNES_HEIGHT_EXTENDED * 128);
    if (!s9x_port_objonline)
        s9x_port_objonline = (uint8_t *)port_alloc_sram((size_t)SNES_HEIGHT_EXTENDED * 128);

    GFX.Screen     = strip_screen;
    GFX.SubScreen  = strip_sub;
    GFX.ZBuffer    = strip_z;
    GFX.SubZBuffer = strip_subz;

    /* Set the strides explicitly as well, rather than trusting the derivation.
     * If a future allocation makes the bases non-adjacent again, this is the
     * line that keeps the rasteriser writing inside its own scanline instead of
     * leaving it to a pointer subtraction to notice. */
    GFX.Delta      = SNES_STRIP_W;
    GFX.DepthDelta = SNES_STRIP_W;

    s9x_update_interleave_field();

    return s9x_port_objonline != nullptr;
}

/* Free the four strips as the two allocations they actually are. Freeing the
 * halves separately would be a double free on the pair blocks. */
extern "C" void S9xDeinitDisplay(void)
{
    port_alloc_free(strip_screen); strip_screen = nullptr;   /* head of pair16 */
    port_alloc_free(strip_z);      strip_z      = nullptr;   /* head of pair8  */
    strip_sub = strip_subz = nullptr;   /* interior pointers into the pairs */
    port_alloc_free(s9x_port_objonline); s9x_port_objonline = nullptr;
    GFX.Screen = GFX.SubScreen = GFX.ZBuffer = GFX.SubZBuffer = nullptr;
}

extern "C" uint32_t S9xReadJoypad(int32_t port)
{
    /* One player, eight GPIO buttons, already combo-filtered and already mapped
     * to SNES bits by main.cpp. snes9x's S9xUpdateJoypads handles the rest. */
    if (port == 0) return s_current_pad_mask;
    return 0;
}

/* No mouse or SuperScope on this console. */
extern "C" bool S9xReadMousePosition(int32_t, int32_t *, int32_t *, uint32_t *)       { return false; }
extern "C" bool S9xReadSuperScopePosition(int32_t *, int32_t *, uint32_t *)          { return false; }
extern "C" bool JustifierOffscreen(void)                                            { return true; }
extern "C" void JustifierButtons(uint32_t *)                                        { }
extern "C" void S9xToggleSoundChannel(int32_t)                                      { }
/* S9xNextController is defined by snes9x's own ppu.c. */

#if MIX_ON_CORE1
/* Cross-core sound-state lock. snes9x's APU mixes on core1 while the emulator
 * runs on core0, and both sides touch the S-DSP state. Neither holder takes this
 * from an IRQ, so spin_lock_unsafe is correct here and an irq_save variant is
 * not needed. */
#include "pico/sync.h"
static spin_lock_t *snd_lock;

extern "C" void port_sound_lock_init(void)
{
    if (!snd_lock)
        snd_lock = spin_lock_instance(spin_lock_claim_unused(true));
}
extern "C" void __not_in_flash_func(port_sound_lock)(void)
{
    spin_lock_unsafe_blocking(snd_lock);
}
extern "C" void __not_in_flash_func(port_sound_unlock)(void)
{
    spin_unlock_unsafe(snd_lock);
}
#endif