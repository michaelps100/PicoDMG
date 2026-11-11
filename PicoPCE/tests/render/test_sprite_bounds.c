// Run the actual renderer with the single-line OSD and the R3 BSS adjacency.
// The expected pixels are decoded independently, one bitplane at a time.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../pce-go/gfx.c"

PCE_t PCE;
static uint16_t vram[0x8000];
static sprite_t sprites[64];
static uint8_t arena[XBUF_WIDTH * (XBUF_HEIGHT + 2) + 512] __attribute__((aligned(8)));
static uint64_t masks[XBUF_HEIGHT];

static uint8_t *render(int x, int y, int row, int width, int height, int hf, int vf) {
    int height_code = height == 64 ? 2 : height == 32 ? 1 : 0;
    memset(sprites, 0, sizeof(sprites));
    sprites[0].x = x + 32;
    sprites[0].y = y + 64;
    sprites[0].attr = ((width == 32) << 8) | (height_code << 12) |
                      (hf ? H_FLIP : 0) | (vf ? V_FLIP : 0);
    vdc_ctx_t ctx = {PCE.VDC.regs, vram, sprites};
    build_sprite_lists(&ctx, masks);
    uint8_t *visible = arena + 80 + row * XBUF_WIDTH;
    // Match main.cpp's virtual base: exactly one physical scanline is writable.
    uint8_t *virtual_base = visible - row * XBUF_WIDTH;
    framebuffer_top = virtual_base - 16;
    framebuffer_bottom = virtual_base + 240 * XBUF_WIDTH;
    draw_sprites(&ctx, masks, virtual_base, row, row + 1, 0);
    return visible;
}

static uint8_t expected_pixel(int screen_x, int row, int x, int y, int width, int height, int hf, int vf) {
    int sx = screen_x - x, sy = row - y;
    if (sx < 0 || sx >= width || sy < 0 || sy >= height) return 0xa5;
    if (hf) sx = width - 1 - sx;
    if (vf) sy = height - 1 - sy;
    unsigned cell = (sy / 16) * 128 + (sx / 16) * 64;
    unsigned bit = 15 - sx % 16;
    unsigned color = 0;
    for (unsigned plane = 0; plane < 4; ++plane)
        color |= ((vram[cell + sy % 16 + plane * 16] >> bit) & 1u) << plane;
    return color ? PCE.Palette[256 + color] : 0xa5;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    memset(arena, 0xa5, sizeof(arena));
    memset(vram, 0, sizeof(vram));
    memset(PCE.Palette, 0, sizeof(PCE.Palette)); // opaque black sprite pixels
    // First 16x16 cell uses color 1, second cell transparent.
    for (unsigned i = 0; i < 16; ++i) vram[i] = 0xffff;
    const int row = 100;
    uint8_t *visible = arena + 80 + row * XBUF_WIDTH;
    // R3 ELF: audio pointer 0x2001B084, visible line 0x2001B0A4 (32 bytes later).
    uint32_t pointer = 0x200735a0;
    memcpy(visible - 32, &pointer, sizeof(pointer));
    render(-29, row, row, 32, 16, 0, 0);
    memcpy(&pointer, visible - 32, sizeof(pointer));
    printf("Audio pointer after off-left sprite: %08X (expected 200735A0)\n", pointer);
    fflush(stdout);
    if (pointer != 0x200735a0) return 1;
    puts("PASS R3 crash regression: off-left cell cannot corrupt adjacent audio pointer");

    // A mix of transparent, fully opaque and sparse pixels exercises both the
    // scalar and packed store paths. Reference decode does not use the fast path.
    memset(vram, 0, sizeof(vram));
    for (unsigned cell = 0; cell < 8; ++cell) {
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                unsigned color = (x * 3 + y * 5 + cell * 7) & 15;
                for (unsigned p = 0; p < 4; ++p)
                    vram[cell * 64 + y + p * 16] |= ((color >> p) & 1u) << (15 - x);
            }
        }
    }
    for (unsigned i = 1; i < 16; ++i) PCE.Palette[256 + i] = i * 11;
    const int lines[] = {0, 13, 120, 239};
    const int heights[] = {16, 32, 64};
    unsigned cases = 0;
    for (unsigned l = 0; l < sizeof(lines)/sizeof(lines[0]); ++l)
    for (unsigned h = 0; h < sizeof(heights)/sizeof(heights[0]); ++h)
    for (int width = 16; width <= 32; width += 16)
    for (int hf = 0; hf < 2; ++hf)
    for (int vf = 0; vf < 2; ++vf)
    for (int sample = 0; sample < 3; ++sample)
    for (int x = -32; x < XBUF_WIDTH + 16; ++x) {
        int line = lines[l], height = heights[h];
        int sy = sample == 0 ? 0 : sample == 1 ? height / 2 : height - 1;
        int y = line - sy;
        memset(arena, 0xa5, sizeof(arena));
        uint8_t *pixels = render(x, y, line, width, height, hf, vf);
        for (int p = -80; p < -16; ++p) assert(pixels[p] == 0xa5);
        for (int p = XBUF_WIDTH + 48; p < XBUF_WIDTH + 96; ++p) assert(pixels[p] == 0xa5);
        // XBUF_WIDTH includes 16 right-hand scratch pixels; the maximum
        // visible PCE line is 352 pixels (pce-go.h). Scratch output is free
        // to differ at the last framebuffer row, but visible pixels are not.
        for (int p = 0; p < XBUF_WIDTH - 16; ++p) {
            uint8_t expected = expected_pixel(p, line, x, y, width, height, hf, vf);
            if (pixels[p] != expected) {
                fprintf(stderr, "pixel mismatch x=%d width=%d height=%d H=%d V=%d row=%d sy=%d pixel=%d got=%u expected=%u\n",
                        x, width, height, hf, vf, line, sy, p, pixels[p], expected);
                return 2;
            }
        }
        ++cases;
    }
    printf("PASS %u sprite cases: borders, both widths, all heights, both flips, transparency and visible pixels\n", cases);
}
