/* Reference runner: the MCUME/Virtual VCS core from third_party, driven with
 * the same pad script as a26run, writing PNG frames and RAM per frame. Used
 * only to compare game behaviour against core2600. Never part of firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "types.h"
#include "address.h"
#include "vmachine.h"
#include "display.h"

void vcs_Init(void);
extern void mainloop(void);

enum { B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_FIRE = 16, B_SELECT = 32, B_RESET = 64 };
static unsigned s_pad;
static int s_frame, s_png_at = -1;
static uint16_t s_pal[256];
static const char *s_out = ".";
static FILE *s_csv;

/* ---- emu API ---- */
/* Generous slack: the legacy raster writes past VBuf on frames taller than 192. */
void *emu_Malloc(int size) { return calloc(1, (size_t)size * 4 + 65536); }
void emu_printf(const char *s) { (void)s; }
void emu_sndInit(void) {}
void emu_sndPlaySound(int c, int v, int f) { (void)c; (void)v; (void)f; }
int emu_LoadFile(char *name, char *buf, int size) {
    FILE *f = fopen(name, "rb"); if (!f) return 0;
    int n = (int)fread(buf, 1, (size_t)size, f); fclose(f); return n;
}
void emu_SetPaletteEntry(unsigned char r, unsigned char g, unsigned char b, int i) {
    if (i >= 0 && i < 256) s_pal[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ---- PNG (stored deflate) ---- */
static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c; }
}
static uint32_t crc_upd(uint32_t c, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8], c4[4]; be32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc_upd(0xffffffffu, (const uint8_t *)t, 4); c = crc_upd(c, d, n) ^ 0xffffffffu;
    be32(c4, c); fwrite(c4, 1, 4, f);
}
static void write_png(const char *path, const unsigned char *idx, int w0, int h) {
    int w = w0 * 2; size_t raw_n = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(raw_n), *p = raw;
    for (int y = 0; y < h; y++) { *p++ = 0;
        for (int x = 0; x < w; x++) { uint16_t c = s_pal[idx[y * w0 + x / 2]];
            *p++ = (uint8_t)(((c >> 11) & 31) * 255 / 31); *p++ = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            *p++ = (uint8_t)((c & 31) * 255 / 31); } }
    size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_n + blocks * 5 + 4), *q = z; *q++ = 0x78; *q++ = 0x01;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t len = raw_n - off > 65535 ? 65535 : raw_n - off;
        *q++ = (uint8_t)(off + len == raw_n); *q++ = (uint8_t)(len & 255); *q++ = (uint8_t)(len >> 8);
        *q++ = (uint8_t)(~len & 255); *q++ = (uint8_t)((~len >> 8) & 255);
        memcpy(q, raw + off, len); q += len; }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(q, (b << 16) | a); q += 4;
    FILE *f = fopen(path, "wb"); if (!f) return;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)(q - z)); chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
}

void emu_DrawScreenPal16(unsigned char *buf, int w, int h, int stride) {
    (void)stride;
    if (s_frame == s_png_at) {
        char path[1024]; snprintf(path, sizeof path, "%s/ref%05d.png", s_out, s_frame);
        write_png(path, buf, w, h);
    }
}

/* ---- input (replaces Keyboard.c; standard 2600 bit layout) ---- */
void keyjoy(void) {
    uint8_t v = 0xff;
    if (s_pad & B_RIGHT) v &= (uint8_t)~0x80;
    if (s_pad & B_LEFT)  v &= (uint8_t)~0x40;
    if (s_pad & B_DOWN)  v &= (uint8_t)~0x20;
    if (s_pad & B_UP)    v &= (uint8_t)~0x10;
    riotRead[0x280] = v;
}
void keytrig(void) {
    if (!(tiaWrite[VBLANK] & 0x40)) {
        tiaRead[INPT5] = 0x80;
        tiaRead[INPT4] = (s_pad & B_FIRE) ? 0x00 : 0x80;
    } else if (s_pad & B_FIRE) tiaRead[INPT4] = 0x00;
}
void keycons(void) {
    riotRead[SWCHB] = 0x3f; /* both difficulties A? use B: bits 6,7 = 0; colour; switches up */
    if (s_pad & B_RESET) riotRead[SWCHB] &= 0xfe;
    if (s_pad & B_SELECT) riotRead[SWCHB] &= 0xfd;
}

/* ---- script ---- */
typedef struct { int from, to; unsigned mask; } seg_t;
static seg_t s_segs[64]; static int s_nsegs;
static unsigned btn(const char *n, size_t l) {
    static const struct { const char *s; unsigned b; } t[] = {{"up", B_UP}, {"down", B_DOWN}, {"left", B_LEFT},
        {"right", B_RIGHT}, {"fire", B_FIRE}, {"select", B_SELECT}, {"reset", B_RESET}, {"start", B_RESET}, {"none", 0}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strlen(t[i].s) == l && !strncmp(n, t[i].s, l)) return t[i].b;
    fprintf(stderr, "bad button\n"); exit(2);
}
static void parse(const char *s) {
    char buf[2048], *ctx = NULL; snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok_s(buf, ";", &ctx); tok && s_nsegs < 64; tok = strtok_s(NULL, ";", &ctx)) {
        seg_t g = {0, 0, 0}; char names[256] = "";
        if (sscanf_s(tok, "%d-%d:%255s", &g.from, &g.to, names, (unsigned)sizeof names) != 3) exit(2);
        for (char *n = names; *n;) { char *e = strchr(n, '+'); size_t l = e ? (size_t)(e - n) : strlen(n);
            g.mask |= btn(n, l); n += l; if (*n == '+') n++; }
        s_segs[s_nsegs++] = g;
    }
}
static unsigned pad_at(int fr) {
    unsigned m = 0;
    for (int i = 0; i < s_nsegs; i++) if (fr >= s_segs[i].from && fr <= s_segs[i].to) m |= s_segs[i].mask;
    return m;
}

void emu_DrawVsync(void) {
    if (s_csv) {
        fprintf(s_csv, "%d,%02x,", s_frame, s_pad);
        for (int i = 0; i < 128; i++) fprintf(s_csv, "%02x", theRam[i]);
        fprintf(s_csv, "\n");
    }
    s_frame++;
    s_pad = pad_at(s_frame);
}

int main(int argc, char **argv) {
    const char *rom = NULL, *script = ""; int frames = 300;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--png-at") && i + 1 < argc) s_png_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) s_out = argv[++i];
        else rom = argv[i];
    }
    if (!rom) return 2;
    crc_init(); parse(script);
    char path[1024]; snprintf(path, sizeof path, "%s/ref_ram.csv", s_out);
    s_csv = fopen(path, "w");
    vcs_Init();
    FILE *f = fopen(rom, "rb"); if (!f) return 1;
    size_t n = fread(theCart, 1, 16384, f); fclose(f);
    rom_size = (int)n;
    if (n == 2048) { memcpy(&theCart[2048], &theCart[0], 2048); rom_size = 4096; }
    init_hardware(); init_banking();
    s_pad = pad_at(0);
    while (s_frame < frames) mainloop();
    if (s_csv) fclose(s_csv);
    printf("ref frames=%d\n", s_frame);
    return 0;
}
