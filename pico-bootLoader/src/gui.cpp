#include "gui.h"

#include <cstdio>
#include <cstring>

#include "FrensFonts.h"
#include "FrensHelpers.h"
#include "ff.h"
#include "image_convert.h"

namespace {
    uint16_t *s_buf_cur          = nullptr;
    uint16_t *s_buf_next         = nullptr;
    bool      s_next_is_half_res = false;   // true on SRAM-only configs
    const char *s_footer         = nullptr; // one-line button-hint overlay
    char      s_asset_dir[64]    = "/emu";  // overridable via gui_set_asset_dir()

    // Directory of the active artwork theme, and of theme 0 which acts as the
    // fallback when the active theme has no artwork for a key. Both are seeded
    // from s_asset_dir so gui works even if the theme setters are never called.
    char      s_theme_dir[80]    = "/emu/assets/themes/0";
    char      s_theme_fb_dir[80] = "/emu/assets/themes/0";
}

// Half-resolution dimensions for the slide-only buffer used when there is
// no PSRAM. 160x120 = 38,400 bytes, 4x smaller than the full image.
#define GUI_HALF_W (SCREENWIDTH  / 2)
#define GUI_HALF_H (SCREENHEIGHT / 2)

// Footer band: one 8-pixel character row at the very bottom of the screen.
#define GUI_FOOTER_H   FONT_CHAR_HEIGHT                  // 8
#define GUI_FOOTER_Y0  (SCREENHEIGHT - GUI_FOOTER_H)     // 232

// Backend-specific 16-bit pixel encodings for the footer band -- same
// per-backend split main.cpp uses for the error overlay (RGB555 on HSTX,
// RGB444 packed 0x0RGB on PicoDVI).
#if HSTX
#define GUI_FOOTER_FG  0x7FFFu
#define GUI_FOOTER_BG  0x0000u
#elif USE_ILI9341
#define GUI_FOOTER_FG  0xFFFFu          // graphical buffers are RGB565 here
#define GUI_FOOTER_BG  0x0000u
#else
#define GUI_FOOTER_FG  0x0FFFu
#define GUI_FOOTER_BG  0x0000u
#endif

extern "C" {

// Returns the saved mode. When the file is absent or unreadable the default
// is graphical -- that's the experience first-time users should land on.
// Read-only now: /boot.txt's GUI key owns the setting, and main.cpp calls this
// once to migrate a legacy card before deleting the file.
bool gui_load_mode(const char *path)
{
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return true;
    char c = '1';
    UINT br = 0;
    f_read(&fil, &c, 1, &br);
    f_close(&fil);
    if (br != 1) return true;
    return c != '0';
}

void gui_set_footer(const char *text) { s_footer = text; }

void gui_set_asset_dir(const char *dir)
{
    if (!dir || !*dir) return;
    size_t n = strlen(dir);
    if (n >= sizeof(s_asset_dir)) n = sizeof(s_asset_dir) - 1;
    memcpy(s_asset_dir, dir, n);
    s_asset_dir[n] = '\0';

    // Keep the theme dirs consistent with the new base until the caller
    // overrides them with the theme it actually resolved.
    snprintf(s_theme_dir,    sizeof(s_theme_dir),    "%s/assets/themes/0", s_asset_dir);
    snprintf(s_theme_fb_dir, sizeof(s_theme_fb_dir), "%s/assets/themes/0", s_asset_dir);
}

void gui_set_theme_dir(const char *dir)
{
    if (!dir || !*dir) return;
    snprintf(s_theme_dir, sizeof(s_theme_dir), "%s", dir);
}

void gui_set_theme_fallback_dir(const char *dir)
{
    if (!dir || !*dir) { s_theme_fb_dir[0] = '\0'; return; }
    snprintf(s_theme_fb_dir, sizeof(s_theme_fb_dir), "%s", dir);
}

bool gui_buffers_alloc(void)
{
    const size_t sz_full = SCREENWIDTH * SCREENHEIGHT * sizeof(uint16_t);
    if (!s_buf_cur)  s_buf_cur  = (uint16_t *)Frens::f_malloc(sz_full);
    if (!s_buf_cur) {
        printf("[bootLoader] gui: cur buffer alloc failed (size=%u)\n", (unsigned)sz_full);
        return false;
    }
    // Second buffer enables the slide animation. With PSRAM we mirror cur
    // at full resolution and the swap-pointers-after-slide pattern works.
    // Without PSRAM we'd OOM (215 KB base + 2x150 KB > 512 KB SRAM), so
    // allocate the next buffer at half res -- still enough to drive a
    // slide, just visibly chunkier. The caller reloads cur at full res
    // after the slide to restore the static-display sharpness.
    if (!s_buf_next) {
        if (Frens::isPsramEnabled()) {
            s_buf_next = (uint16_t *)Frens::f_malloc(sz_full);
            s_next_is_half_res = false;
        } else {
            const size_t sz_half = GUI_HALF_W * GUI_HALF_H * sizeof(uint16_t);
            s_buf_next = (uint16_t *)Frens::f_malloc(sz_half);
            s_next_is_half_res = true;
        }
    }
    if (!s_buf_next) {
        printf("[bootLoader] gui: slide buffer alloc failed; animation disabled\n");
    } else {
        printf("[bootLoader] gui: buffers ready (cur=full, next=%s)\n",
               s_next_is_half_res ? "half" : "full");
    }
    return true;
}

bool gui_next_is_half_res(void) { return s_next_is_half_res; }

uint16_t *gui_buf_cur(void)  { return s_buf_cur;  }
uint16_t *gui_buf_next(void) { return s_buf_next; }

void gui_swap_buffers(void)
{
    uint16_t *t = s_buf_cur;
    s_buf_cur  = s_buf_next;
    s_buf_next = t;
}

void gui_fill_solid(uint16_t *dest, uint16_t color)
{
    if (!dest) return;
    const size_t pixels = SCREENWIDTH * SCREENHEIGHT;
    for (size_t i = 0; i < pixels; i++) dest[i] = color;
}

void gui_fill_solid_half_res(uint16_t *dest, uint16_t color)
{
    if (!dest) return;
    const size_t pixels = GUI_HALF_W * GUI_HALF_H;
    for (size_t i = 0; i < pixels; i++) dest[i] = color;
}

// Open the artwork for image_key. On SPI-panel builds a native RGB565
// "<dir>/<key>.565" is tried first in each theme dir (no conversion, any size
// up to 320x240 -- 266x240 fills the GB_MODE window exactly). Otherwise
// "<dir>/<key><FILEXTFORSEARCH>", converting a same-named PNG/JPG on demand.
// When the active theme has no artwork for this key, falls back to theme 0.
// Returns true with *fil open at offset 0; *is_565 says which format it is.
static bool gui_open_asset(const char *image_key, FIL *fil, bool *is_565)
{
    const char *dirs[2] = { s_theme_dir, s_theme_fb_dir };
    char path[FF_MAX_LFN + 1];

    for (int i = 0; i < 2; i++) {
        const char *dir = dirs[i];
        if (!dir || !*dir) continue;
        if (i == 1 && strcmp(dir, dirs[0]) == 0) break;   // fallback == active

#if USE_ILI9341
        snprintf(path, sizeof(path), "%s/%s.565", dir, image_key);
        if (f_open(fil, path, FA_READ) == FR_OK) { *is_565 = true; return true; }
#endif
        image_convert_ensure(dir, image_key, SCREENWIDTH, SCREENHEIGHT, /*letterbox=*/true);
        snprintf(path, sizeof(path), "%s/%s%s", dir, image_key, FILEXTFORSEARCH);
        if (f_open(fil, path, FA_READ) == FR_OK) { *is_565 = false; return true; }
    }

    printf("[bootLoader] gui: no artwork for '%s' in %s (or theme 0)\n",
           image_key, s_theme_dir);
    return false;
}

uint16_t gui_color(uint16_t c444)
{
#if USE_ILI9341
    // The graphical-menu buffers hold RGB565 on SPI-panel builds.
    const uint16_t r = (c444 >> 8) & 0x0F, g = (c444 >> 4) & 0x0F, b = c444 & 0x0F;
    return (uint16_t)((((r << 1) | (r >> 3)) << 11) |
                      (((g << 2) | (g >> 2)) << 5)  |
                       ((b << 1) | (b >> 3)));
#else
    return c444;
#endif
}

// Read a w x h RGB565 image (fil positioned just past the header) and place it
// centred in the visible window of a 320x240 screen, black around it. The
// window is x=45..310 in GB_MODE, the full width otherwise. With half_res the
// screen is decimated 2:1 into a 160x120 dest on the fly. Reads one source
// row at a time, so no large scratch buffer is needed.
static bool gui_read_565(FIL *fil, const char *image_key, int w, int h,
                         uint16_t *dest, bool half_res)
{
    int vx = 0, vw = SCREENWIDTH;
#if USE_ILI9341
    if (ili9341_get_gb_mode()) { vx = 45; vw = 266; }
#endif
    if (w > vw) { vx = 0; vw = SCREENWIDTH; }   // wider than the window: centre on screen
    const int x0 = vx + (vw - w) / 2;
    const int y0 = (SCREENHEIGHT - h) / 2;

    if (half_res) gui_fill_solid_half_res(dest, 0);
    else          gui_fill_solid(dest, 0);

    uint16_t row[SCREENWIDTH];   // 640 bytes on the stack
    const UINT row_bytes = (UINT)w * sizeof(uint16_t);
    for (int r = 0; r < h; r++) {
        UINT br = 0;
        FRESULT fr = f_read(fil, row, row_bytes, &br);
        if (fr != FR_OK || br != row_bytes) {
            printf("[bootLoader] gui: '%s' short read row %d\n", image_key, r);
            return false;
        }
        const int y = y0 + r;
        if (!half_res) {
            memcpy(dest + y * SCREENWIDTH + x0, row, row_bytes);
        } else if ((y & 1) == 0) {
            uint16_t *out = dest + (y >> 1) * GUI_HALF_W;
            for (int xo = (x0 + 1) >> 1; 2 * xo < x0 + w; xo++) out[xo] = row[2 * xo - x0];
        }
    }
    return true;
}

// Shared by both loaders: open the asset, validate the header, then read it
// either as native RGB565 (placed in the window) or as the legacy full-screen
// .444/.555 format.
static bool gui_load_any(const char *image_key, uint16_t *dest, bool half_res)
{
    if (!dest || !image_key || !*image_key) return false;

    FIL fil;
    bool is_565 = false;
    if (!gui_open_asset(image_key, &fil, &is_565)) return false;

    uint16_t hdr[2];
    UINT br = 0;
    FRESULT fr = f_read(&fil, hdr, sizeof(hdr), &br);
    if (fr != FR_OK || br != sizeof(hdr)) {
        printf("[bootLoader] gui: '%s' header read failed (fr=%d br=%u)\n",
               image_key, (int)fr, (unsigned)br);
        f_close(&fil);
        return false;
    }

    if (is_565) {
        const int w = hdr[0], h = hdr[1];
        if (w < 1 || h < 1 || w > SCREENWIDTH || h > SCREENHEIGHT) {
            printf("[bootLoader] gui: '%s.565' is %d x %d, max %d x %d\n",
                   image_key, w, h, SCREENWIDTH, SCREENHEIGHT);
            f_close(&fil);
            return false;
        }
        bool ok = gui_read_565(&fil, image_key, w, h, dest, half_res);
        f_close(&fil);
        return ok;
    }

    if (hdr[0] != SCREENWIDTH || hdr[1] != SCREENHEIGHT) {
        printf("[bootLoader] gui: '%s' is %u x %u, expected %d x %d\n",
               image_key, (unsigned)hdr[0], (unsigned)hdr[1], SCREENWIDTH, SCREENHEIGHT);
        f_close(&fil);
        return false;
    }

    size_t pixels;
    if (!half_res) {
        const UINT want = SCREENWIDTH * SCREENHEIGHT * sizeof(uint16_t);
        fr = f_read(&fil, dest, want, &br);
        if (fr != FR_OK || br != want) {
            printf("[bootLoader] gui: '%s' pixel read short (fr=%d br=%u/%u)\n",
                   image_key, (int)fr, (unsigned)br, (unsigned)want);
            f_close(&fil);
            return false;
        }
        pixels = SCREENWIDTH * SCREENHEIGHT;
    } else {
        // Downsample 2:1 in each axis, one scanline at a time.
        uint16_t row[SCREENWIDTH];
        const UINT row_bytes = SCREENWIDTH * sizeof(uint16_t);
        for (int y = 0; y < SCREENHEIGHT; y++) {
            fr = f_read(&fil, row, row_bytes, &br);
            if (fr != FR_OK || br != row_bytes) {
                printf("[bootLoader] gui: '%s' short read row %d\n", image_key, y);
                f_close(&fil);
                return false;
            }
            if (y & 1) continue;   // nearest-neighbour vertical decimation
            uint16_t *out = dest + (y >> 1) * GUI_HALF_W;
            for (int x = 0; x < GUI_HALF_W; x++) out[x] = row[2 * x];
        }
        pixels = GUI_HALF_W * GUI_HALF_H;
    }
    f_close(&fil);

#if USE_ILI9341
    // Legacy RGB444 artwork: widen to RGB565 so every graphical-menu buffer
    // has the same format and the panel gets it through unchanged.
    for (size_t i = 0; i < pixels; i++) dest[i] = gui_color(dest[i]);
#else
    (void)pixels;
#endif
    return true;
}

bool gui_load_image(const char *image_key, uint16_t *dest)
{
    return gui_load_any(image_key, dest, false);
}

bool gui_load_image_half_res(const char *image_key, uint16_t *dest)
{
    return gui_load_any(image_key, dest, true);
}

// Helper: write n pixels of b's scanline into dst, starting at b-column
// b_start. b is the full-resolution row at output-row y, so this is just
// a memcpy.
static inline void copy_b_full(uint16_t *dst, const uint16_t *brow,
                               int b_start, int n)
{
    memcpy(dst, brow + b_start, n * sizeof(uint16_t));
}

// Helper: write n pixels of half-res b into dst, starting at output-column
// b_start in 320-px space. b's row is shared between two output rows
// (y/2) and each b pixel covers two output columns. Each output column
// x reads brow_half[x / 2].
static inline void copy_b_half(uint16_t *dst, const uint16_t *brow_half,
                               int b_start, int n)
{
    for (int i = 0; i < n; i++) {
        dst[i] = brow_half[(b_start + i) >> 1];
    }
}

// Paint one scanline of the footer band on top of whatever compose_row()
// just wrote into dst. y is in [GUI_FOOTER_Y0, SCREENHEIGHT). Fills the
// whole row with bg so the band is opaque, then overlays glyph pixels from
// the shared 8x8 font (same pattern as menu.cpp's charcell pipeline).
static void overlay_footer_row(uint16_t *dst, int y, const char *text)
{
    const uint16_t fg = GUI_FOOTER_FG;
    const uint16_t bg = GUI_FOOTER_BG;

    for (int x = 0; x < SCREENWIDTH; x++) dst[x] = bg;

    const int len      = (int)strlen(text);
    const int text_w   = len * FONT_CHAR_WIDTH;
    const int x_start  = (SCREENWIDTH - text_w) / 2;
    const int row_in_g = y - GUI_FOOTER_Y0;   // 0..7 within the glyph

    if (x_start < 0 || text_w > SCREENWIDTH) return;   // string too wide

    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < FONT_FIRST_ASCII || c >= FONT_FIRST_ASCII + FONT_N_CHARS)
            c = '?';
        char slice = getcharslicefrom8x8font((char)c, row_in_g);
        uint16_t *out = dst + x_start + i * FONT_CHAR_WIDTH;
        for (int bit = 0; bit < 8; bit++) {
            out[bit] = (slice & 1) ? fg : bg;
            slice >>= 1;
        }
    }
}

// Per-scanline composition. `a` is always full 320x240. `b` may be NULL,
// 320x240 (b_half_res=false), or 160x120 (b_half_res=true).
static inline void compose_row(uint16_t *dst, int y,
                               const uint16_t *a, const uint16_t *b,
                               int p, int dir, bool b_half_res)
{
    const uint16_t *arow = a + y * SCREENWIDTH;

    if (!b || p <= 0 || dir == 0) {
        memcpy(dst, arow, SCREENWIDTH * sizeof(uint16_t));
        return;
    }

    // Locate b's row for this output scanline.
    const uint16_t *brow = b_half_res
        ? (b + (y >> 1) * GUI_HALF_W)
        : (b + y * SCREENWIDTH);

    if (p >= SCREENWIDTH) {
        // All b.
        if (b_half_res) copy_b_half(dst, brow, 0, SCREENWIDTH);
        else            copy_b_full(dst, brow, 0, SCREENWIDTH);
    } else if (dir > 0) {
        // b slides in from the right. Old image scrolls left.
        // dst = arow[p .. W-1] then b mapped to columns 0 .. p-1.
        memcpy(dst, arow + p, (SCREENWIDTH - p) * sizeof(uint16_t));
        if (b_half_res) copy_b_half(dst + (SCREENWIDTH - p), brow, 0, p);
        else            copy_b_full(dst + (SCREENWIDTH - p), brow, 0, p);
    } else {
        // b slides in from the left. Old image scrolls right.
        // dst = b mapped to columns W-p .. W-1 then arow[0 .. W-1-p].
        if (b_half_res) copy_b_half(dst, brow, SCREENWIDTH - p, p);
        else            copy_b_full(dst, brow, SCREENWIDTH - p, p);
        memcpy(dst + p, arow, (SCREENWIDTH - p) * sizeof(uint16_t));
    }
}

#if USE_ILI9341
// ---------------------------------------------------------------------------
// Hardware-scrolled slide. The old image is already in panel GRAM, so the
// incoming image only needs to be in one buffer, and each step sends just the
// columns it reveals (a few KB) instead of the whole frame. The panel offset
// changes in one register write, so a step tears at most along one line by a
// few pixels, instead of the staircase a 25 ms full-frame write produced.
// ---------------------------------------------------------------------------
namespace {
    int             s_hw_x   = 0;      // window left edge on screen
    int             s_hw_w   = SCREENWIDTH;
    const uint16_t *s_hw_img = nullptr;
}

// Footer pixel for screen column x of footer row y, same layout as
// overlay_footer_row() so the strips match the static frame exactly.
static inline uint16_t footer_px(int x, int y, const char *text, int len, int x_start)
{
    const int i = (x - x_start) >> 3;
    if (x < x_start || i >= len) return GUI_FOOTER_BG;
    unsigned char c = (unsigned char)text[i];
    if (c < FONT_FIRST_ASCII || c >= FONT_FIRST_ASCII + FONT_N_CHARS) c = '?';
    const char slice = getcharslicefrom8x8font((char)c, y - GUI_FOOTER_Y0);
    return ((slice >> ((x - x_start) & 7)) & 1) ? GUI_FOOTER_FG : GUI_FOOTER_BG;
}

static void hw_slide_row(int y, uint16_t *dst, int x0, int n, void *)
{
    memcpy(dst, s_hw_img + y * SCREENWIDTH + x0, (size_t)n * sizeof(uint16_t));
    if (s_footer && *s_footer && y >= GUI_FOOTER_Y0) {
        const int len = (int)strlen(s_footer);
        const int tw  = len * FONT_CHAR_WIDTH;
        if (tw > SCREENWIDTH) {
            for (int i = 0; i < n; i++) dst[i] = GUI_FOOTER_BG;
            return;
        }
        const int xs = (SCREENWIDTH - tw) / 2;
        for (int i = 0; i < n; i++) dst[i] = footer_px(x0 + i, y, s_footer, len, xs);
    }
}

bool gui_hw_slide_begin(const uint16_t *incoming)
{
    if (!incoming) return false;
    s_hw_x = 0; s_hw_w = SCREENWIDTH;
    if (ili9341_get_gb_mode()) { s_hw_x = 45; s_hw_w = 266; }
    if (!ili9341_hscroll_begin(s_hw_x, s_hw_w)) return false;
    s_hw_img = incoming;
    return true;
}

int gui_hw_slide_width(void) { return s_hw_w; }

void gui_hw_slide_step(int p_old, int p_new, int direction)
{
    if (!s_hw_img || p_new <= p_old) return;
    if (p_new > s_hw_w) p_new = s_hw_w;
    const int W = s_hw_w;
    // Point the panel at the new offset first: the strip about to be written
    // then sits at the edge the new image enters from, never on the side the
    // old image is still showing.
    ili9341_hscroll_set(direction > 0 ? p_new : W - p_new);
    if (direction > 0) {
        ili9341_write_cols_565(s_hw_x + p_old, s_hw_x + p_new - 1, hw_slide_row, nullptr);
    } else {
        ili9341_write_cols_565(s_hw_x + W - p_new, s_hw_x + W - p_old - 1, hw_slide_row, nullptr);
    }
}

void gui_hw_slide_end(void)
{
    // At p == W the window holds the incoming image at its natural columns,
    // so dropping the offset back to 0 changes nothing on screen.
    ili9341_hscroll_end();
    s_hw_img = nullptr;
}
#endif

void gui_draw_frame(const uint16_t *a, const uint16_t *b,
                    int slide_px, int direction, bool b_half_res)
{
    if (!a) return;
    if (slide_px < 0) slide_px = 0;
    if (slide_px > SCREENWIDTH) slide_px = SCREENWIDTH;

    for (int y = 0; y < SCREENHEIGHT; y++) {
        uint16_t *dst;
#if USE_ILI9341
        dst = &Frens::framebuffer[y * SCREENWIDTH];
#elif !HSTX
        dvi::DVI::LineBuffer *lb = nullptr;
#if FRAMEBUFFERISPOSSIBLE
        if (Frens::isFrameBufferUsed()) {
            dst = &Frens::framebuffer[y * SCREENWIDTH];
        } else
#endif
        {
            lb  = dvi_->getLineBuffer();
            dst = lb->data();
        }
#else
        dst = hstx_getlineFromFramebuffer(y);
#endif

        compose_row(dst, y, a, b, slide_px, direction, b_half_res);

        // Footer band: composed into dst before the line-stream push so
        // every backend (HSTX FB, PicoDVI FB, PicoDVI line-stream) gets it.
        if (s_footer && *s_footer && y >= GUI_FOOTER_Y0) {
            overlay_footer_row(dst, y, s_footer);
        }

#if !HSTX && !USE_ILI9341
#if FRAMEBUFFERISPOSSIBLE
        if (!Frens::isFrameBufferUsed())
#endif
            dvi_->setLineBuffer(y, lb);
#endif
    }
    // Note: On ILI9341, ili9341_flush(true) is invoked by the caller (main.cpp)
    // after any active overlays (such as the volume OSD HUD) are composited onto Frens::framebuffer,
    // avoiding double-flush flickering.
}

} // extern "C"
