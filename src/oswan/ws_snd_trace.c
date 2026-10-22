/**
 * ws_snd_trace.c - TEMPORARY sound/timer/interrupt trace (see ws_snd_trace.h)
 */
#include "ws_snd_trace.h"

#if WS_SND_TRACE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "types.h"
#include "io.h"
#include "gpu.h"

extern uint8 internalRam[64 << 10];

enum { TR_OUT = 1, TR_SUM_IRQ, TR_SUM_DRV, TR_SUM_MIX };

/* 16 bytes, so 32 KB of PSRAM per 2048 entries */
typedef struct {
    uint32_t frame;
    uint8_t  kind;
    uint8_t  line;
    uint8_t  a, b;
    uint8_t  d[8];
} tr_entry_t;

bool ws_trace_on = false;

static tr_entry_t *s_buf;
static uint32_t    s_cap, s_head, s_count;
static uint32_t    s_frame;

/* last value logged per port: sound registers are rewritten every driver tick with the
 * same values, so only changes are worth an entry */
static uint8_t s_shadow[256];
static uint8_t s_shadow_valid[32];   /* bitmap */

/* per-frame accumulators */
static uint8_t s_irq[8];
static uint8_t s_lost_h, s_lost_v;
static uint8_t s_n89, s_min89, s_max89;

static tr_entry_t *tr_push(uint8_t kind) {
    tr_entry_t *e = &s_buf[s_head];
    if (++s_head == s_cap) s_head = 0;
    if (s_count < s_cap) s_count++;
    memset(e, 0, sizeof(*e));
    e->frame = s_frame;
    e->kind  = kind;
    e->line  = ws_gpu_scanline;
    return e;
}

static void tr_frame_clear(void) {
    memset(s_irq, 0, sizeof(s_irq));
    s_lost_h = s_lost_v = 0;
    s_n89 = 0;
    s_min89 = 0xFF;
    s_max89 = 0;
}

void ws_trace_attach(void *buf, size_t bytes) {
    s_buf = (tr_entry_t *)buf;
    s_cap = (uint32_t)(bytes / sizeof(tr_entry_t));
    ws_trace_reset();
}

void ws_trace_reset(void) {
    if (!s_buf || !s_cap) { ws_trace_on = false; return; }
    s_head = s_count = 0;
    s_frame = 0;
    memset(s_shadow_valid, 0, sizeof(s_shadow_valid));
    tr_frame_clear();
    ws_trace_on = true;
}

static inline uint8_t sat_inc(uint8_t v) { return v == 0xFF ? v : (uint8_t)(v + 1); }

void ws_trace_port_slow(uint8_t port, uint8_t value) {
    /* the voice stream: one sample per line, summarised per frame */
    if (port == 0x89 && (ws_ioRam[0x90] & 0x20)) {
        s_n89 = sat_inc(s_n89);
        if (value < s_min89) s_min89 = value;
        if (value > s_max89) s_max89 = value;
        return;
    }
    const bool sound = (port >= 0x80 && port <= 0x95) || port == 0x52 || port == 0x6A || port == 0x6B;
    const bool timer = (port >= 0xA2 && port <= 0xAB) || port == 0xB0 || port == 0xB2 || port == 0xB6;
    if (!sound && !timer) return;
    if (port == 0xB6 && (value == 0x80 || value == 0x40)) return;   /* routine acks */
    if (sound) {
        const uint8_t bit = (uint8_t)(1u << (port & 7));
        if ((s_shadow_valid[port >> 3] & bit) && s_shadow[port] == value) return;
        s_shadow[port] = value;
        s_shadow_valid[port >> 3] |= bit;
    }
    tr_entry_t *e = tr_push(TR_OUT);
    e->a = port;
    e->b = value;
}

void ws_trace_irq_slow(int level) {
    if (level >= 0 && level < 8) s_irq[level] = sat_inc(s_irq[level]);
}

void ws_trace_timer_fire_slow(int level, bool was_pending) {
    if (!was_pending) return;
    if (level == 7) s_lost_h = sat_inc(s_lost_h);
    else            s_lost_v = sat_inc(s_lost_v);
}

void ws_trace_frame_end(void) {
    tr_entry_t *e = tr_push(TR_SUM_IRQ);
    e->a = s_irq[7];
    e->b = s_irq[6];
    e->d[0] = s_lost_h;
    e->d[1] = s_lost_v;
    e->d[2] = s_n89;
    e->d[3] = s_n89 ? s_min89 : 0;
    e->d[4] = s_max89;
    e->d[5] = ws_ioRam[0x90];
    e->d[6] = ws_ioRam[0x94];
    e->d[7] = (uint8_t)(s_irq[0] + s_irq[1] + s_irq[2] + s_irq[3] + s_irq[4] + s_irq[5]);

    e = tr_push(TR_SUM_DRV);
    e->a = ws_ioRam[0xA2];
    e->b = ws_ioRam[0xB2];
    e->d[0] = ws_ioRam[0xB6];
    e->d[1] = internalRam[0x262];
    e->d[2] = internalRam[0x264];
    e->d[3] = internalRam[0x271];
    e->d[4] = internalRam[0x277];
    e->d[5] = internalRam[0x268];
    e->d[6] = internalRam[0x266];
    e->d[7] = internalRam[0x267];

    ws_audio_trace_t m;
    ws_audio_trace_take(&m);
    e = tr_push(TR_SUM_MIX);
    e->a = (uint8_t)m.clipped;
    e->b = (uint8_t)(m.clipped >> 8);
    e->d[0] = (uint8_t)m.mix_peak;
    e->d[1] = (uint8_t)(m.mix_peak >> 8);
    e->d[2] = m.ch_peak[0];
    e->d[3] = m.ch_peak[1];
    e->d[4] = m.ch_peak[2];
    e->d[5] = m.ch_peak[3];
    e->d[6] = m.hv_peak;
    e->d[7] = ws_ioRam[0x89];

    tr_frame_clear();
    s_frame++;
}

/* ---------------------------------------------------------------------------------- */
static FIL  s_f;
static char s_txt[1024];
static UINT s_len;
static bool s_ok;

static void txt_flush(void) {
    UINT bw = 0;
    if (s_ok && s_len && (f_write(&s_f, s_txt, s_len, &bw) != FR_OK || bw != s_len)) s_ok = false;
    s_len = 0;
}

static void txt_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void txt_add(const char *fmt, ...) {
    if (s_len > sizeof(s_txt) - 200) txt_flush();
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_txt + s_len, sizeof(s_txt) - s_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += (UINT)n;
}

bool ws_trace_dump(const char *path) {
    if (!s_buf || !s_count) return false;
    const bool was_on = ws_trace_on;
    ws_trace_on = false;
    if (f_open(&s_f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("[trace] cannot open %s\n", path);
        ws_trace_on = was_on;
        return false;
    }
    s_ok = true;
    s_len = 0;
    txt_add("# PicoWS sound trace: %lu entries, frames up to %lu\n",
            (unsigned long)s_count, (unsigned long)s_frame);
    txt_add("# OUT = port write (sound ports: changes only; 0x89 in voice mode summarised)\n");
    txt_add("# SUM irq H/V = HBlank-timer / VBlank interrupts taken, lost = timer fired while still pending,\n");
    txt_add("#     n89 = voice samples written [min..max], ctl = port 90, v94 = port 94, oth = other irqs\n");
    txt_add("#     drv = RAM 262 264 271 277, ptr = 268:267 266 | mix clip = clipped samples, pk = peak |mix|,\n");
    txt_add("#     ch = peak |output| ch1..4, hv = hyper voice peak, r89 = port 89 now\n");

    uint32_t idx = (s_head + s_cap - s_count) % s_cap;
    for (uint32_t n = 0; n < s_count; n++) {
        const tr_entry_t *e = &s_buf[idx];
        if (++idx == s_cap) idx = 0;
        switch (e->kind) {
        case TR_OUT:
            txt_add("F=%06lu L=%03u OUT %02X=%02X\n", (unsigned long)e->frame, e->line, e->a, e->b);
            break;
        case TR_SUM_IRQ:
            txt_add("F=%06lu SUM irq H=%3u V=%u oth=%u lost H=%u V=%u | n89=%3u [%02X..%02X] ctl=%02X v94=%02X",
                    (unsigned long)e->frame, e->a, e->b, e->d[7], e->d[0], e->d[1],
                    e->d[2], e->d[3], e->d[4], e->d[5], e->d[6]);
            break;
        case TR_SUM_DRV:
            txt_add(" | a2=%02X b2=%02X b6=%02X drv=%02X %02X %02X %02X ptr=%02X:%02X%02X",
                    e->a, e->b, e->d[0], e->d[1], e->d[2], e->d[3], e->d[4], e->d[5], e->d[7], e->d[6]);
            break;
        case TR_SUM_MIX:
            txt_add(" | mix clip=%u pk=%u ch=%u/%u/%u/%u hv=%u r89=%02X\n",
                    e->a | (e->b << 8), e->d[0] | (e->d[1] << 8),
                    e->d[2], e->d[3], e->d[4], e->d[5], e->d[6], e->d[7]);
            break;
        default:
            break;
        }
    }
    txt_flush();
    const bool ok = s_ok && f_close(&s_f) == FR_OK;
    printf("[trace] %s: %s (%lu entries)\n", path, ok ? "written" : "write failed", (unsigned long)s_count);
    ws_trace_on = was_on;
    return ok;
}

#endif /* WS_SND_TRACE */
