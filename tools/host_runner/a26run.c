/* Headless host runner for the PicoA26 core: runs a ROM with a scripted pad,
 * writes PNG frames, a 44.1 kHz WAV (the firmware's audio_cb path) and a
 * per-frame CSV with a framebuffer hash. Evidence tool, not part of firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "a26_core.h"
#include "atari2600emulator.h"
#include "core_state.h"
#include "atari/atari_audio.h"

#define FB_W 160
#define FB_H 192
static uint16_t s_fb[FB_H][FB_W];
static int16_t *s_wav;
static size_t s_wav_n, s_wav_cap;
static int s_volume = 3;

static void line_cb(int line, const uint16_t *px) {
    if (line < 0 || line >= FB_H) return;
    if (px) memcpy(s_fb[line], px, sizeof(s_fb[line]));
    else memset(s_fb[line], 0, sizeof(s_fb[line]));
}

static void audio_cb(void) {
    if (s_wav_n == s_wav_cap) {
        s_wav_cap = s_wav_cap ? s_wav_cap * 2 : (size_t)1 << 20;
        s_wav = realloc(s_wav, s_wav_cap * sizeof(int16_t));
    }
    s_wav[s_wav_n++] = atari_audio_get_sample((uint8_t)s_volume);
}

/* ---- minimal PNG writer (stored deflate blocks) ---- */
static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_upd(uint32_t c, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hdr[8], t[4];
    be32(hdr, n); memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc_upd(0xffffffffu, (const uint8_t *)type, 4);
    c = crc_upd(c, data, n) ^ 0xffffffffu;
    be32(t, c); fwrite(t, 1, 4, f);
}
static void write_png(const char *path, int scale) {
    int w = FB_W * 2 * scale, h = FB_H * scale; /* 2:1 pixel aspect, like the TV */
    size_t raw_n = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(raw_n), *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) {
            uint16_t c = s_fb[y / scale][x / (2 * scale)];
            *p++ = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            *p++ = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            *p++ = (uint8_t)((c & 31) * 255 / 31);
        }
    }
    size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_n + blocks * 5 + 4), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t len = raw_n - off > 65535 ? 65535 : raw_n - off;
        *q++ = (uint8_t)(off + len == raw_n);
        *q++ = (uint8_t)(len & 255); *q++ = (uint8_t)(len >> 8);
        *q++ = (uint8_t)(~len & 255); *q++ = (uint8_t)((~len >> 8) & 255);
        memcpy(q, raw + off, len); q += len;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(q, (b << 16) | a); q += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
}

static void le32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void write_wav(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t data = (uint32_t)(s_wav_n * 2), rate = A26_AUDIO_HZ;
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); le32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16); h[20] = 1; h[22] = 1; le32(h + 24, rate); le32(h + 28, rate * 2);
    h[32] = 2; h[34] = 16; memcpy(h + 36, "data", 4); le32(h + 40, data);
    fwrite(h, 1, 44, f); fwrite(s_wav, 2, s_wav_n, f); fclose(f);
}

/* ---- input script: "0-60:none;61-70:reset;100-900:fire+left" ---- */
typedef struct { int from, to; unsigned mask; } seg_t;
enum { B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_FIRE = 16, B_SELECT = 32, B_RESET = 64 };
static seg_t s_segs[64];
static int s_nsegs;
static unsigned btn_bit(const char *n, size_t l) {
    static const struct { const char *name; unsigned bit; } names[] = {
        {"up", B_UP}, {"down", B_DOWN}, {"left", B_LEFT}, {"right", B_RIGHT}, {"fire", B_FIRE},
        {"select", B_SELECT}, {"reset", B_RESET}, {"start", B_RESET}, {"none", 0}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i].name) == l && !strncmp(n, names[i].name, l)) return names[i].bit;
    fprintf(stderr, "bad button '%.*s'\n", (int)l, n);
    exit(2);
}
static void parse_script(const char *s) {
    char buf[2048], *ctx = NULL;
    snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok_s(buf, ";", &ctx); tok && s_nsegs < 64; tok = strtok_s(NULL, ";", &ctx)) {
        seg_t g = {0, 0, 0};
        char names[256] = "";
        if (sscanf_s(tok, "%d-%d:%255s", &g.from, &g.to, names, (unsigned)sizeof names) != 3) {
            fprintf(stderr, "bad segment '%s'\n", tok);
            exit(2);
        }
        for (char *n = names; *n;) {
            char *e = strchr(n, '+');
            size_t l = e ? (size_t)(e - n) : strlen(n);
            g.mask |= btn_bit(n, l);
            n += l;
            if (*n == '+') n++;
        }
        s_segs[s_nsegs++] = g;
    }
}
static unsigned pad_at(int frame) {
    unsigned m = 0;
    for (int i = 0; i < s_nsegs; i++)
        if (frame >= s_segs[i].from && frame <= s_segs[i].to) m |= s_segs[i].mask;
    return m;
}
static uint32_t fnv(const void *d, size_t n) {
    uint32_t h = 2166136261u;
    const uint8_t *p = d;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static bool in_list(const char *list, int fr) {
    char buf[1024], *ctx = NULL;
    snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok_s(buf, ",", &ctx); t; t = strtok_s(NULL, ",", &ctx))
        if (atoi(t) == fr) return true;
    return false;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *script = "", *out = ".", *pngs = "";
    int frames = 300, every = 0, scale = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--png-every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png-at") && i + 1 < argc) pngs = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) s_volume = atoi(argv[++i]);
        else rom_path = argv[i];
    }
    if (!rom_path) {
        fprintf(stderr, "usage: a26run rom [--frames N] [--input S] [--png-every K] [--png-at a,b] [--out dir]\n");
        return 2;
    }
    crc_init();
    parse_script(script);
    FILE *f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); return 1; }
    static uint8_t rom[65536 + 512];
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    a26_core_init();
    if (!a26_core_load_rom(rom, n)) { fprintf(stderr, "load failed: %s\n", a26_core_last_error()); return 1; }
    a26_core_set_region_hint(rom_path);

    char path[1024];
    snprintf(path, sizeof path, "%s/frames.csv", out);
    FILE *csv = fopen(path, "w");
    if (!csv) { perror(path); return 1; }
    fprintf(csv, "frame,pad,scanlines,clocks,reason,hash,ram80_8f\n");
    uint32_t total = 2166136261u;
    for (int fr = 0; fr < frames; fr++) {
        unsigned m = pad_at(fr);
        a26_core_set_controls(m & B_UP, m & B_DOWN, m & B_LEFT, m & B_RIGHT, m & B_FIRE, m & B_SELECT, m & B_RESET);
        a26_frame_result_t r = a26_core_run_frame(line_cb, audio_cb);
        uint32_t h = fnv(s_fb, sizeof s_fb);
        total = (total ^ h) * 16777619u;
        fprintf(csv, "%d,%02x,%u,%u,%d,%08x,", fr, m, r.scanlines, r.color_clocks, (int)r.reason, h);
        for (int i = 0; i < (getenv("A26_RAM") ? 128 : 16); i++) fprintf(csv, "%02x", memory[i]);
        fprintf(csv, "\n");
        if ((every > 0 && fr % every == 0) || (pngs[0] && in_list(pngs, fr))) {
            snprintf(path, sizeof path, "%s/f%05d.png", out, fr);
            write_png(path, scale);
        }
        if (r.reason == A26_FRAME_JAM) { printf("JAM at frame %d pc=%04x\n", fr, cpu.pc); break; }
    }
    fclose(csv);
    snprintf(path, sizeof path, "%s/audio.wav", out);
    write_wav(path);
    printf("frames=%d total_hash=%08x samples=%zu\n", frames, total, s_wav_n);
    return 0;
}
