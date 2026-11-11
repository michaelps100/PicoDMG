//=============================================================================
// NGP on-screen profiler counters.
//
// Deliberately built out of metrics that are cheap enough to leave enabled:
// nothing here is incremented once per emulated instruction, because that
// would cost more than the frame it is trying to measure.
//
// NGP_PROFILE_TIMERS is the one exception. Counting updateTimers_slow() entries
// really does cost a load/add/store per instruction -- on the order of 10 ms a
// frame -- so it is compile-time optional and OFF by default. The cheap way to
// get that same number is a diagnostic build with the timer-clock block in
// interrupt.c compiled out, and compare frame times.
//
// The counters are plain externs rather than a shared header: the display
// driver is compiled as C and would otherwise need src/neopop on its include
// path, and this keeps the build files untouched.
//=============================================================================

#ifndef __NGP_PROF__
#define __NGP_PROF__

/* Off by default: the panel is a diagnostic, not a feature. Build with
 * -DNGP_PROFILE=1 to bring it back. It costs nothing when disabled -- every
 * counter and every panel stamp is behind this guard. */
#ifndef NGP_PROFILE
#define NGP_PROFILE 0
#endif

#include "NeoPop.h"
#ifdef __cplusplus
extern "C" {
#endif

// 64-instruction blocks retired by emulate(). Multiply by 64 for instructions.
extern volatile _u32 ngp_prof_blocks;
// Exact count of TLCS-900h instructions retired. Counted in a register inside
// emulate()'s loop, so it is free -- unlike the block count it does not
// assume every block retired a full 64.
extern volatile _u32 ngp_prof_ins;
// Value of TRUN (ram[0x20] & 0x0F), the hardware timer enable register. This
// is the decisive one: updateTimers()'s inline fast path is
// `timer_hint >= TIMER_HINT_RATE || (ram[0x20] & 0x0F)`, so any non-zero TRUN
// makes the "fast path" unreachable and calls updateTimers_slow() on every
// single instruction. Read once per emulate() call, so it costs nothing.
extern volatile _u32 ngp_prof_trun;
// 1 when the Z80 is running this frame (ram[0xb9] == 0x55).
extern volatile _u32 ngp_prof_z80on;
// ExecZ80_batch() calls (NeoPop only runs the Z80 in slices).
extern volatile _u32 ngp_prof_z80;
// gfx_draw() entries, i.e. rasterised scanlines per frame.
extern volatile _u32 ngp_prof_gfx;
// Native 160 px lines handed to the display transport.
extern volatile _u32 ngp_prof_lines;
// Wall-clock microseconds spent inside the emulate() phase of one frame,
// measured by the caller in main.cpp. This is the number that says whether the
// budget goes to the emulated CPU or to everything around it. It is a running
// per-second average, and deliberately NOT cleared by ngp_prof_reset().
extern volatile _u32 ngp_prof_emu_us;
// updateTimers_slow() entries. Only meaningful with NGP_PROFILE_TIMERS=1.
extern volatile _u32 ngp_prof_timers;

// Microseconds Core 0 spent blocked on a full display ring (inside EMU).
extern volatile _u32 ngp_prof_stall_us;
// Microseconds accumulated in the scanline rasteriser / in ExecZ80_batch.
extern volatile _u32 ngp_prof_gfx_us;
extern volatile _u32 ngp_prof_z80_us;

#if NGP_PROFILE
#include "pico/time.h"
#endif

void ngp_prof_reset(void);

#ifdef __cplusplus
}
#endif

#endif
