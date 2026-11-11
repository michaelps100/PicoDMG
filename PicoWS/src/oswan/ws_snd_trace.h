/**
 * ws_snd_trace.h - sound/timer/interrupt trace (diagnostic, OFF by default)
 *
 * Records, in a PSRAM ring buffer, what the game writes to the sound, timer and interrupt
 * ports, plus a per-frame summary (interrupts taken / lost, voice samples streamed, driver
 * state bytes, mixer clipping and per-channel peaks). Opening the in-game menu dumps it as
 * text to 0:/saves/ws/klonoa_trace.txt.
 *
 * This is how the Klonoa "music changes after grabbing the Moo" bug was found (AGENTS.md
 * section 30). Enable with -DWS_EXTRA_DEFS="WS_SND_TRACE=1" on a PSRAM build; otherwise
 * every hook is an empty inline and nothing is allocated.
 */
#ifndef __WS_SND_TRACE_H__
#define __WS_SND_TRACE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef WS_SND_TRACE
#define WS_SND_TRACE 0
#endif
#if WS_SND_TRACE && !(defined(PSRAM_CS_PIN) && PSRAM_CS_PIN)
#undef WS_SND_TRACE
#define WS_SND_TRACE 0      /* the ring buffer lives in PSRAM */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if WS_SND_TRACE
extern bool ws_trace_on;

void ws_trace_attach(void *buf, size_t bytes);
void ws_trace_reset(void);
void ws_trace_port_slow(uint8_t port, uint8_t value);
void ws_trace_irq_slow(int level);
void ws_trace_timer_fire_slow(int level, bool was_pending);
void ws_trace_frame_end(void);
bool ws_trace_dump(const char *path);

/* mixer statistics, accumulated by ws_audio.c and collected at frame end */
typedef struct {
    uint16_t clipped;      /* output samples that hit +/-32767 */
    uint16_t mix_peak;     /* largest |mix| before master volume */
    uint8_t  ch_peak[4];   /* largest |channel output| per channel */
    uint8_t  hv_peak;      /* largest |hyper voice| */
} ws_audio_trace_t;
void ws_audio_trace_take(ws_audio_trace_t *out);

static inline void ws_trace_port(uint8_t port, uint8_t value) {
    if (ws_trace_on) ws_trace_port_slow(port, value);
}
static inline void ws_trace_irq(int level) {
    if (ws_trace_on) ws_trace_irq_slow(level);
}
static inline void ws_trace_timer_fire(int level, bool was_pending) {
    if (ws_trace_on) ws_trace_timer_fire_slow(level, was_pending);
}
#else
static inline void ws_trace_reset(void) {}
static inline void ws_trace_port(uint8_t port, uint8_t value) { (void)port; (void)value; }
static inline void ws_trace_irq(int level) { (void)level; }
static inline void ws_trace_timer_fire(int level, bool was_pending) { (void)level; (void)was_pending; }
static inline void ws_trace_frame_end(void) {}
static inline bool ws_trace_dump(const char *path) { (void)path; return false; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* __WS_SND_TRACE_H__ */
