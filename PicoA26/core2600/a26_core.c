#include "atari2600emulator.h"
#include "a26_core.h"
#include "core_state.h"
#include "atari/atari_audio.h"
#include "platform.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_CART_SIZE 65536

AudioChannel left, right;
TV tv = { .x = 160, .y = 0, .standard = TV_NTSC };

static uint8_t s_cart_rom[MAX_CART_SIZE];
static size_t  s_cart_size = 0;
static uint16_t s_line_buf[160];
static bool s_line_active;

/* Divides the colour clock into phi2 machine cycles (tv.x / 3).
 *
 * Declared at file scope, not inside a26_core_run_frame(), because
 * a26_core_reset() has to be able to clear it: it is the CPU/beam phase, and
 * carrying a non-zero value across a reset permanently re-phases the CPU
 * against the beam (which also throws off the HMOVE table, indexed off tv.x).
 * A function-static could not be reset from outside. */
static uint8_t s_phi2_cnt = 0;
/* phi2 edges whose timer/audio work is still pending, see a26_catch_up(). */
static uint32_t s_lazy_cycles;
static uint64_t s_color_clocks_total = 0;
static uint32_t s_sample_phase = 0;
static uint8_t s_audio_divider = 0;
static int s_region_hint = -1;
static uint8_t s_region_votes;
static int s_region_candidate;
static uint16_t s_capture_height = 192;
uint32_t a26_core_color_clock_hz(void) {
    return tv.standard==TV_PAL || tv.standard==TV_SECAM ? A26_PAL_COLOR_HZ : A26_NTSC_COLOR_HZ;
}
void a26_core_set_region_hint(const char *filename) {
    char upper[256]; size_t i=0;
    if(filename) for(;filename[i] && i<sizeof(upper)-1;++i)
        upper[i]=(char)toupper((unsigned char)filename[i]);
    upper[i]=0;
    s_region_hint=-1;
    if(strstr(upper,"SECAM")) s_region_hint=TV_SECAM;
    else if(strstr(upper,"PAL-M") || strstr(upper,"PAL M")) s_region_hint=TV_NTSC;
    else if(strstr(upper,"PAL") || strstr(upper,"(E)") || strstr(upper,"(EUROPE)")) s_region_hint=TV_PAL;
    else if(strstr(upper,"NTSC") || strstr(upper,"(U)") || strstr(upper,"(USA)")) s_region_hint=TV_NTSC;
    s_region_votes=0;
    video_set_standard(s_region_hint<0?TV_NTSC:s_region_hint);
}
static uint32_t s_rom_crc;
static uint32_t state_crc(const uint8_t *data,size_t n);
static const char *s_load_error = "";
const char *a26_core_last_error(void) { return s_load_error; }


/* =========================================================================
 * Audio Callbacks (forward to hardware TIA synthesizer in atari_audio.c)
 * ========================================================================= */
void A26_RAMFUNC(audio_AUDC0_write)(uint8_t value) { atari_audio_set_reg(0x15, value); }
void A26_RAMFUNC(audio_AUDC1_write)(uint8_t value) { atari_audio_set_reg(0x16, value); }
void A26_RAMFUNC(audio_AUDF0_write)(uint8_t value) { atari_audio_set_reg(0x17, value); }
void A26_RAMFUNC(audio_AUDF1_write)(uint8_t value) { atari_audio_set_reg(0x18, value); }
void A26_RAMFUNC(audio_AUDV0_write)(uint8_t value) { atari_audio_set_reg(0x19, value); }
void A26_RAMFUNC(audio_AUDV1_write)(uint8_t value) { atari_audio_set_reg(0x1A, value); }
void audio_init(void) { atari_audio_init(); }

/* =========================================================================
 * Core Initialization & ROM Loading
 * ========================================================================= */
uint8_t *a26_core_get_rom_buffer(size_t *max_size) {
    if (max_size) *max_size = sizeof(s_cart_rom);
    return s_cart_rom;
}

void a26_core_init(void) {
    s_cart_size = cartridge_size = 0;
    memset(&cartridge,0,sizeof(cartridge));
    tv.x = 0;
    tv.y = 0;
    tv.standard = TV_NTSC;
    input_mode = INPUT_JOYSTICK;
    a26_core_reset();
}

void a26_core_reset(void) {
    memset(memory,0,sizeof(memory));
    memset(&cpu,0,sizeof(cpu));
    cpu.s=0xfd; cpu.flag_i=true;
    cartridge_reset();
    video_init_objects();
    input_reset_ports();
    delayed_writer_reset();
    timer_reset();
    audio_init();
    memset(s_line_buf,0,sizeof(s_line_buf));
    s_line_active=false;
    tv.x=0; tv.y=0; tv.standard=s_region_hint<0?TV_NTSC:s_region_hint;
    vsync=false; vblank=true; wsync=false;
    s_phi2_cnt=0; s_color_clocks_total=0; s_lazy_cycles=0;
    a26_video_rebase();
    s_sample_phase=0; s_audio_divider=0;
    s_region_votes=0; s_region_candidate=tv.standard; s_capture_height=192;
    cpu_go_to_reset_vector();
}

bool a26_core_load_rom(const uint8_t *rom_data, size_t size) {
    if (!rom_data || size == 0) { s_load_error="ROM vazia ou invalida"; return false; }

    /* Strip common headers (128-byte A26 header, 32-byte, 64-byte) */
    if ((size % 1024) == 128 && size > 128) {
        rom_data += 128;
        size -= 128;
    } else if ((size % 1024) == 32 && size > 32) {
        rom_data += 32;
        size -= 32;
    } else if ((size % 1024) == 64 && size > 64) {
        rom_data += 64;
        size -= 64;
    }

    if(size>MAX_CART_SIZE) { s_load_error="ROM maior que 64 KB"; return false; }
    int type=cartridge_detect(rom_data,size);
    if(type<0) {
        s_load_error=type==-2 ? "Mapper nao suportado (DPC/ARM/3E/3F)" : "Tamanho de ROM nao suportado";
        return false;
    }
    if(s_cart_rom!=rom_data) memmove(s_cart_rom,rom_data,size);
    s_cart_size=cartridge_size=size;
    s_rom_crc=state_crc(s_cart_rom,size);
    memset(&cartridge,0,sizeof(cartridge));
    cartridge.type=type; cartridge.raw=s_cart_rom;
    s_load_error="";
    printf("[A26] Mapper %s, %u bytes\n",cartridge_name(type),(unsigned)size);

    s_region_hint=-1;
    tv.standard = TV_NTSC;
    input_mode = INPUT_JOYSTICK;
    a26_core_reset();
    return true;
}

/* =========================================================================
 * Controls Mapping
 * ========================================================================= */
void a26_core_set_controls(bool up, bool down, bool left, bool right, bool fire, bool select, bool reset) {
    uint8_t joy = 0xFF;
    if (right) joy &= ~(1 << 7);
    if (left)  joy &= ~(1 << 6);
    if (down)  joy &= ~(1 << 5);
    if (up)    joy &= ~(1 << 4);

    input_set_physical_controls(joy, fire, false, select, reset);
}

/* =========================================================================
 * Lazy pixel rendering
 * =========================================================================
 * TIA output for a pixel depends only on TIA state, and that state changes
 * only through a TIA register write, a delayed write landing, RSYNC or a
 * scanline wrap. So instead of rendering one pixel per colour clock, pixels
 * are rendered in spans right before any of those events (and before a
 * collision register is read). The result is identical to per-clock
 * rendering; the per-clock loop no longer pays for it. */
static uint8_t s_render_x; /* first pixel of the current line not yet rendered */

void A26_RAMFUNC(a26_video_sync)(void) {
    unsigned end = (unsigned)tv.x < 160u ? (unsigned)tv.x : 160u;
    unsigned x0 = s_render_x;
    if (x0 >= end) return;
    s_render_x = (uint8_t)end;
    if (vblank || vsync) {
        for (unsigned x = x0; x < end; ++x) s_line_buf[x] = 0;
    } else {
        video_render_span(x0, end, s_line_buf);
        s_line_active = true;
    }
}

/* tv.x moved without the beam passing the pixels in between (RSYNC, reset,
 * state load): restart the render pointer at the new beam position. */
void a26_video_rebase(void) {
    s_render_x = (unsigned)tv.x < 160u ? (uint8_t)tv.x : 160u;
}

/* =========================================================================
 * Frame Execution
 * ========================================================================= */
typedef struct {
    void (*line_cb)(int line, const uint16_t *pixels);
    void (*audio_cb)(void);
    uint32_t color_hz;
    int visible_line, output_lines;
    uint32_t line_watchdog;
} a26_frame_ctx_t;

/* Called when tv.x reaches 228. */
static inline void end_scanline(a26_frame_ctx_t *c) {
    a26_video_sync();
    tv.x = 0;
    tv.y++;
    c->line_watchdog++;

    /* A completed scanline is forward progress. SPI waits do not
     * feed the watchdog; the frame also has an independent clock cap.
     * Fed every 32 lines (~2 ms against a 6 s timeout): the SDK call
     * lives in flash, and 262 RAM->flash calls a frame bought nothing. */
    if ((c->line_watchdog & 31u) == 0) watchdog_update();

    video_begin_scanline();

    /* Preserve a partially visible line even if VBLANK changed in
     * HBLANK. Blanked pixels must not reuse the previous scanline. */
    if (s_line_active) {
        /* Fit the measured image into the existing display buffer. */
        int dest=c->visible_line*192/s_capture_height;
        if (dest < 192) {
            if (c->line_cb) c->line_cb(dest, s_line_buf);
            if(dest+1>c->output_lines) c->output_lines=dest+1;
        }
        ++c->visible_line;
    }
    s_line_active=false;
    s_render_x=0;
}

/* Timer and audio are only observable through RIOT accesses, AUDx writes and
 * the end of the frame, so their per-phi2 work is deferred: each edge just
 * counts, and a26_catch_up() replays the pending edges -- in the same order
 * and with the same arithmetic as running them one by one -- right before any
 * of those observation points. */
static void (*s_frame_audio_cb)(void);
static uint32_t s_frame_color_hz = A26_NTSC_COLOR_HZ;

void A26_RAMFUNC(a26_catch_up)(void) {
    uint32_t n = s_lazy_cycles;
    if (n == 0) return;
    s_lazy_cycles = 0;

    /* Audio, event by event. Per cycle the original did: divider += 3 and a
     * TIA step when it reaches 114 (every 38 cycles), then phase += 3*44100
     * and a sample when it reaches the colour clock. Jumping to the next of
     * those two events keeps the exact interleaving (step before sample when
     * both land on the same cycle). */
    const uint32_t inc = 3u * A26_AUDIO_HZ, hz = s_frame_color_hz;
    uint32_t left = n;
    while (left) {
        uint32_t to_step = (114u - s_audio_divider) / 3u;          /* >= 1 */
        uint32_t to_sample = s_sample_phase + inc >= hz ? 1u
                           : (hz - s_sample_phase + inc - 1u) / inc; /* >= 1 */
        uint32_t k = to_step < to_sample ? to_step : to_sample;
        if (k > left) k = left;
        s_audio_divider = (uint8_t)(s_audio_divider + 3u * k);
        s_sample_phase += inc * k;
        left -= k;
        if (s_audio_divider == 114) { s_audio_divider = 0; atari_audio_step(); }
        if (s_sample_phase >= hz) {
            s_sample_phase -= hz;
            if (s_frame_audio_cb) s_frame_audio_cb();
        }
    }

    /* Timer (timer_tick() n times). Every tick clears the wrapped flag, then
     * counts the prescaler down; an underflow decrements INTIM, and the first
     * wrap past zero latches bit 7, after which the prescaler is 1. */
    left = n;
    timer_wrapped_this_cycle = false;
    while (left) {
        if (timer_underflow_bit7 && timer_secondary_value == 1) {
            /* secondary is 1 here: every remaining tick decrements INTIM. */
            uint8_t before = timer_primary_value;
            timer_primary_value = (uint8_t)(before - left);
            timer_wrapped_this_cycle = timer_primary_value == 255;
            timer_secondary_value = 1;
            break;
        }
        if ((uint32_t)timer_secondary_value > left) {
            timer_secondary_value -= (int)left;
            timer_wrapped_this_cycle = false;
            break;
        }
        left -= (uint32_t)timer_secondary_value;
        --timer_primary_value;
        timer_wrapped_this_cycle = false;
        if (timer_primary_value == 255) { timer_underflow_bit7 = true; timer_wrapped_this_cycle = true; }
        timer_secondary_value = timer_underflow_bit7 ? 1 : timer_prescaler;
    }
}

/* Moves the beam n (<= 228) clocks forward, wrapping the scanline once if it
 * passes 228. Only valid while nothing else is due inside those clocks. */
static inline void advance_beam(a26_frame_ctx_t *c, unsigned n) {
    unsigned nx = (unsigned)tv.x + n;
    if (nx >= 228u) {
        tv.x = 228;
        end_scanline(c);
        nx -= 228u;
    }
    tv.x = (int)nx;
}

a26_frame_result_t __not_in_flash_func(a26_core_run_frame)(void (*line_cb)(int line, const uint16_t *pixels), void (*audio_cb)(void)) {
    a26_frame_ctx_t c = { line_cb, audio_cb, a26_core_color_clock_hz(), 0, 0, 0 };
    s_frame_audio_cb = audio_cb;
    s_frame_color_hz = c.color_hz;
    bool prev_vsync = vsync;

    a26_frame_result_t result={0,0,A26_FRAME_LIMIT};
    while (result.color_clocks < A26_FRAME_CLOCK_LIMIT) {
        /* Fast path: with no delayed write in flight, nothing but the beam
         * moves between two phi2 edges -- the CPU is the only other writer of
         * TIA/RIOT state, VSYNC and the kill latch. So advance straight to the
         * next phi2 edge (at most 3 clocks) and then run the same per-clock
         * tail as the general path below. The clock cap is honoured exactly:
         * near it, the general path takes over one clock at a time. */
        if (s_dw_active == 0 && result.color_clocks + 3u <= A26_FRAME_CLOCK_LIMIT) {
            unsigned k = 3u - s_phi2_cnt; /* clocks up to and including the edge */
            if (wsync) {
                /* WSYNC is released when the beam reaches x == 160. The CPU is
                 * halted until then, so jump the whole wait: only the beam and
                 * the deferred timer/audio edges move. An edge landing on the
                 * release clock is counted without a CPU cycle, as before. */
                unsigned d = tv.x < 160 ? 160u - (unsigned)tv.x : 388u - (unsigned)tv.x;
                if (result.color_clocks + d <= A26_FRAME_CLOCK_LIMIT) {
                    unsigned phase = s_phi2_cnt + d;
                    advance_beam(&c, d);
                    result.color_clocks += d;
                    s_lazy_cycles += phase / 3u;
                    s_phi2_cnt = (uint8_t)(phase % 3u);
                    wsync = false;
                    prev_vsync = vsync;
                    continue;
                }
            }
            advance_beam(&c, k);
            result.color_clocks += k;
            prev_vsync = vsync;
            s_phi2_cnt = 0;
            ++s_lazy_cycles;
            if (!wsync) cpu_run_for_one_machine_cycle();
            if (__builtin_expect(s_dw_active > 0, 0)) tick_delayed_writes();
            if (tv.x == 160 && wsync) wsync = false;
            if(cpu.is_killed) { result.reason=A26_FRAME_JAM; break; }
            if (prev_vsync && !vsync) {
                tv.y = 0;
                result.reason=A26_FRAME_VSYNC;
                break;
            }
            continue;
        }

        ++result.color_clocks;

        /* 1. Advance horizontal beam (pixels are rendered lazily, see
         *    a26_video_sync), 2. scanline wrap at 228 colour clocks */
        if (++tv.x == 228) end_scanline(&c);

        /* 3. Previous VSYNC value for falling-edge detection */
        prev_vsync = vsync;

        /* 4. CPU & Timer tick every 3 color clocks (phi2 machine cycle) */
        if (++s_phi2_cnt == 3) {
            s_phi2_cnt = 0;
            ++s_lazy_cycles;
            if (!wsync) {
                cpu_run_for_one_machine_cycle();
            }
        }

        /* 5. Delayed register writes tick every color clock (only when pending) */
        if (__builtin_expect(s_dw_active > 0, 0)) {
            tick_delayed_writes();
        }

        /* 6. WSYNC released at start of HBLANK (x == 160) */
        if (tv.x == 160 && wsync) {
            wsync = false;
        }

        if(cpu.is_killed) { result.reason=A26_FRAME_JAM; break; }

        /* 7. Falling edge of VSYNC signals end of frame */
        if (prev_vsync && !vsync) {
            tv.y = 0;
            result.reason=A26_FRAME_VSYNC;
            break;
        }
    }
    /* Render what the beam already passed and replay the deferred timer/audio
     * edges, so all state is current for anything that runs between frames. */
    a26_video_sync();
    a26_catch_up();
    int output_lines = c.output_lines, visible_line = c.visible_line;
    uint32_t line_watchdog = c.line_watchdog;

    /* Pad remaining scanlines up to 192 if game produced fewer lines */
    if (line_cb) {
        while (output_lines < 192) {
            line_cb(output_lines, NULL);
            output_lines++;
        }
    }
    if(result.reason==A26_FRAME_VSYNC && line_watchdog>=200) {
        if(visible_line>=192 && visible_line<=320) s_capture_height=visible_line;
        if(s_region_hint<0 && line_watchdog<=350) {
            int region=line_watchdog>285?TV_PAL:TV_NTSC;
            if(region!=s_region_candidate) { s_region_candidate=region; s_region_votes=1; }
            else if(s_region_votes<8) ++s_region_votes;
            if(s_region_votes==8 && tv.standard!=region) {
                video_set_standard(region);
                s_sample_phase=0;
            }
        }
    }
    s_color_clocks_total+=result.color_clocks;
    result.scanlines=line_watchdog;
    return result;
}

#include "state_impl.h"
